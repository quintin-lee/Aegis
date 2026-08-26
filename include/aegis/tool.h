#ifndef AEGIS_TOOL_H
#define AEGIS_TOOL_H

#include "aegis/cancellation.h"
#include "aegis/executor.h"
#include "aegis/task.h"
#include "aegis/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file aegis_tool.h
 * @brief Unified Tool ABI: typed values, arguments, schema, results,
 *        tool definitions, the tool registry, and the tool call runtime.
 *
 * Execution path:
 *
 *     Task -> Executor -> Tool Registry -> Tool -> Result
 *
 * The executor never depends on concrete tools. A task submitted with
 * aegis_tool_submit() runs a bridge work function that resolves the tool
 * by name through the registry, validates arguments against the schema,
 * invokes the tool cooperatively (cancellation token), and stores the
 * result bytes in the task output.
 *
 * ── Ownership model ────────────────────────────────────────────────────
 *  - Arguments (aegis_tool_args_t) OWN copies of every name and every
 *    STRING/BYTES payload added to them.
 *  - A tool definition's name/description/schema parameter strings are
 *    BORROWED from the tool author and must outlive the registration.
 *  - The registry owns its stored copy of aegis_tool_def_t (shallow);
 *    aegis_tool_registry_find() hands out a value copy, so callers never
 *    hold interior pointers into registry storage.
 *  - A tool result OWNS its payload; destroy it exactly once with
 *    aegis_tool_result_destroy().
 *  - A tool job OWNS its tool-name copy and takes TRANSFERRED ownership
 *    of the argument list given to aegis_tool_job_create(); the job is
 *    consumed by the work function after exactly one execution attempt.
 *
 * ── Thread safety ──────────────────────────────────────────────────────
 *  - Registry operations are thread-safe (internal mutex).
 *  - Argument lists and results are NOT thread-safe; confine each to one
 *    thread or add external synchronization.
 *  - Tool execute functions run on executor worker threads with NO locks
 *    held and must honor cooperative cancellation via the token.
 *
 * No transport or encoding is bound by this ABI: there is no HTTP, JSON,
 * or LLM dependency anywhere in the tool interface. Values are a minimal
 * tagged model; task input/output remain raw bytes owned by the task.
 */

/* ── Typed values ─────────────────────────────────────────────────────── */

/** Discriminator for aegis_tool_value_t. */
typedef enum aegis_tool_value_type {
    AEGIS_TOOL_VAL_BOOL   = 0, /**< as.b     */
    AEGIS_TOOL_VAL_INT    = 1, /**< as.i     */
    AEGIS_TOOL_VAL_FLOAT  = 2, /**< as.f     */
    AEGIS_TOOL_VAL_STRING = 3, /**< as.str   */
    AEGIS_TOOL_VAL_BYTES  = 4, /**< as.bytes */
} aegis_tool_value_type_t;

/**
 * @brief Minimal typed value exchanged between caller and tool.
 *
 * Payload ownership depends on context:
 *  - Inside aegis_tool_args_t: STRING/BYTES payloads are owned by the
 *    arg list (deep copies of what was passed in).
 *  - Inside aegis_tool_result_t: the payload is owned by the result.
 *  - Schema specs never carry payloads.
 */
typedef struct aegis_tool_value {
    aegis_tool_value_type_t type;
    union {
        bool    b;
        int64_t i;
        double  f;
        struct {
            const char* ptr; /**< NUL-terminated UTF-8 text. */
            size_t      len; /**< Bytes excluding the NUL.   */
        } str;
        struct {
            const void* ptr; /**< Raw bytes.               */
            size_t      len; /**< Byte count.              */
        } bytes;
    } as;
} aegis_tool_value_t;

/* ── Argument list ────────────────────────────────────────────────────── */

/** Opaque named-argument list. Owns all names and heap payloads. */
typedef struct aegis_tool_args aegis_tool_args_t;

/**
 * @brief Create an empty argument list.
 * @param[out] out Receives the new list on success.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (out NULL), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_tool_args_create(aegis_tool_args_t** out);

/**
 * @brief Destroy an argument list, freeing all names and payloads.
 * @param args List previously created by aegis_tool_args_create(); NULL ok.
 */
void aegis_tool_args_destroy(aegis_tool_args_t* args);

/**
 * @brief Add (copy) a boolean argument under @p name.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_tool_args_add_bool(aegis_tool_args_t* args, const char* name, bool v);

/** Add (copy) a signed 64-bit integer argument. Same errors as add_bool. */
aegis_status_t aegis_tool_args_add_int(aegis_tool_args_t* args, const char* name, int64_t v);

/** Add (copy) a double-precision float argument. Same errors as add_bool. */
aegis_status_t aegis_tool_args_add_float(aegis_tool_args_t* args, const char* name, double v);

/**
 * @brief Add (deep-copy) a string argument. @p s may be NULL only when
 *        @p len == 0, which stores the empty string.
 */
aegis_status_t aegis_tool_args_add_string(aegis_tool_args_t* args, const char* name, const char* s);

/**
 * @brief Add (deep-copy) a raw byte argument. @p data may be NULL only
 *        when @p len == 0.
 */
aegis_status_t aegis_tool_args_add_bytes(aegis_tool_args_t* args, const char* name,
                                         const void* data, size_t len);

/**
 * @brief Look up an argument by exact name match.
 * @param[out] out Receives a borrowed pointer to the stored value
 *                 (valid until the list is destroyed or modified).
 * @return true when found, false otherwise (including NULL inputs).
 */
bool aegis_tool_args_find(const aegis_tool_args_t* args, const char* name,
                          const aegis_tool_value_t** out);

/** Number of arguments stored. NULL-safe (returns 0). */
size_t aegis_tool_args_count(const aegis_tool_args_t* args);

/* ── Schema ───────────────────────────────────────────────────────────── */

/** One parameter specification inside a tool schema. */
typedef struct aegis_tool_param_spec {
    const char*             name;        /**< Borrowed; must outlive the def. */
    aegis_tool_value_type_t type;        /**< Exact required type (no coercion). */
    bool                    required;    /**< Missing required arg fails validation. */
    const char*             description; /**< Optional human-readable hint; may be NULL. */
} aegis_tool_param_spec_t;

/**
 * @brief Structural description of a tool's parameters.
 *
 * The params array is BORROWED from the tool author (typically static
 * storage) and must outlive the tool definition.
 */
typedef struct aegis_tool_schema {
    const aegis_tool_param_spec_t* params; /**< May be NULL when param_count == 0. */
    size_t                         param_count;
} aegis_tool_schema_t;

/**
 * @brief Validate an argument list against a schema.
 *
 * Checks, in order:
 *  1. every supplied argument name exists in the schema (unknown -> fail),
 *  2. every supplied argument has exactly the declared type,
 *  3. every required parameter is present.
 *
 * No coercion is performed: INT does not satisfy FLOAT and vice versa.
 *
 * @param schema Schema to validate against (NULL means empty schema).
 * @param args   Supplied arguments (NULL is treated as an empty list).
 * @return AEGIS_OK when valid; AEGIS_ERR_INVALID on any violation.
 */
aegis_status_t aegis_tool_validate_args(const aegis_tool_schema_t* schema,
                                        const aegis_tool_args_t*   args);

/* ── Result ───────────────────────────────────────────────────────────── */

/**
 * @brief Outcome payload produced by a successful tool execute.
 *
 * The payload is owned by the result and freed by
 * aegis_tool_result_destroy(). A zero-initialized result carries no
 * value and is safe to destroy.
 */
typedef struct aegis_tool_result {
    aegis_tool_value_t value;
} aegis_tool_result_t;

/** Destroy a result and free its payload. NULL-safe; idempotent contents. */
void aegis_tool_result_destroy(aegis_tool_result_t* result);

/** Store a deep-copied string in @p result (replacing any prior payload).
 *  @return AEGIS_OK or AEGIS_ERR_NOMEM. */
aegis_status_t aegis_tool_result_set_string(aegis_tool_result_t* result, const char* s);

/** Store a deep-copied byte buffer in @p result. @p data may be NULL only
 *  when @p len == 0. @return AEGIS_OK or AEGIS_ERR_NOMEM. */
aegis_status_t aegis_tool_result_set_bytes(aegis_tool_result_t* result, const void* data,
                                           size_t len);

/** Store a boolean / int / float by value (no allocation). Always AEGIS_OK. */
aegis_status_t aegis_tool_result_set_bool(aegis_tool_result_t* result, bool v);
aegis_status_t aegis_tool_result_set_int(aegis_tool_result_t* result, int64_t v);
aegis_status_t aegis_tool_result_set_float(aegis_tool_result_t* result, double v);

/* ── Tool definition (the ABI surface a tool author implements) ───────── */

/**
 * @brief Execute entry point of a tool.
 *
 * Runs on an executor worker thread (or the direct caller's thread via
 * aegis_tool_call()) with NO locks held.
 *
 * @param user      The def's user pointer.
 * @param args      Validated arguments (borrowed; do not destroy).
 * @param token     Cooperative cancellation token (borrowed). Long-running
 *                  tools must poll aegis_cancellation_token_is_cancelled()
 *                  and return AEGIS_ERR_CANCELLED promptly when tripped.
 * @param[out] out  Result to fill on success. On success the implementor
 *                  MUST set a value (any helper). On failure the
 *                  implementor MUST leave it zero-valued; the wrapper
 *                  destroys whatever is there either way.
 * @return AEGIS_OK on success; any AEGIS_ERR_* code otherwise. The code
 *         is propagated verbatim to the caller (no remapping).
 */
typedef aegis_status_t (*aegis_tool_execute_fn)(void* user, const aegis_tool_args_t* args,
                                                const aegis_cancellation_token_t* token,
                                                aegis_tool_result_t*              out);

/**
 * @brief Optional asynchronous interrupt hook.
 *
 * Best-effort wake-up for tools whose execute() blocks on external
 * primitives instead of polling the token. May be NULL; cancellation
 * through the token alone is the default contract.
 */
typedef void (*aegis_tool_cancel_fn)(void* user);

/**
 * @brief A registrable tool: metadata + capability requirement + behavior.
 *
 * All strings and the schema param array are borrowed from the tool
 * author and must remain valid for as long as the tool stays registered.
 * The registry stores a shallow copy of this struct.
 */
typedef struct aegis_tool_def {
    const char*           name;         /**< Unique, non-NULL, non-empty. */
    const char*           description;  /**< May be NULL.                 */
    aegis_tool_schema_t   schema;       /**< Parameter contract.          */
    aegis_capability_t    capabilities; /**< Capability mask the tool requires. */
    aegis_tool_execute_fn execute;      /**< Required, non-NULL.          */
    aegis_tool_cancel_fn  cancel;       /**< Optional, may be NULL.       */
    void*                 user;         /**< Passed back to both hooks.   */
} aegis_tool_def_t;

/* ── Registry ─────────────────────────────────────────────────────────── */

/** Opaque name -> tool-definition registry. Thread-safe. */
typedef struct aegis_tool_registry aegis_tool_registry_t;

/**
 * @brief Create an empty registry.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_tool_registry_create(aegis_tool_registry_t** out);

/** Destroy the registry. Stored defs are shallow copies; their borrowed
 *  strings are untouched. NULL-safe. */
void aegis_tool_registry_destroy(aegis_tool_registry_t* reg);

/**
 * @brief Register a tool under its def->name.
 *
 * Stores a shallow copy of @p def. Registration fails if the name is
 * already taken, execute is NULL, or name is empty.
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY (duplicate name),
 *         AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_tool_registry_register(aegis_tool_registry_t*  reg,
                                            const aegis_tool_def_t* def);

/**
 * @brief Resolve a tool by exact name.
 *
 * Copies the stored definition into @p out_def so the caller never holds
 * an interior pointer into registry storage.
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_tool_registry_find(aegis_tool_registry_t* reg, const char* name,
                                        aegis_tool_def_t* out_def);

/** Number of registered tools. NULL-safe (returns 0). */
size_t aegis_tool_registry_count(const aegis_tool_registry_t* reg);

/* ── Call runtime ─────────────────────────────────────────────────────── */

/**
 * @brief Core invocation: resolve -> validate -> execute.
 *
 * Registry lookup and validation happen up front; execute() runs with no
 * locks held. The token must be fully armed by the caller before this
 * call (the executor arms it from the task timeout).
 *
 * On failure the error status propagates verbatim from validation or
 * execute(); @p out is left zeroed whenever the status is not AEGIS_OK.
 *
 * @param reg   Registry to resolve @p name from.
 * @param name  Tool name.
 * @param args  Arguments (validated internally; borrowed).
 * @param token Finalized cancellation/deadline token.
 * @param[out] out Result destination (zeroed first; destroyed content on
 *                 failure paths is handled internally).
 * @return AEGIS_OK, or AEGIS_ERR_NOT_FOUND (unknown tool),
 *         AEGIS_ERR_INVALID (validation), or the tool's own error code.
 */
aegis_status_t aegis_tool_execute(aegis_tool_registry_t* reg, const char* name,
                                  const aegis_tool_args_t*          args,
                                  const aegis_cancellation_token_t* token,
                                  aegis_tool_result_t*              out);

/**
 * @brief Direct convenience call with its own timeout.
 *
 * Builds a stack-local cancellation token, arms the deadline
 * (@p timeout_ms > 0), delegates to aegis_tool_execute(). Use for tools
 * invoked outside the executor; executor-driven calls go through
 * aegis_tool_submit(). On success @p out holds an owned payload that the
 * caller destroys with aegis_tool_result_destroy().
 *
 * @return Same codes as aegis_tool_execute(), plus AEGIS_ERR_TIMEOUT
 *         when the deadline expires and AEGIS_ERR_CANCELLED when the
 *         tool reports cooperative cancellation.
 */
aegis_status_t aegis_tool_call(aegis_tool_registry_t* reg, const char* name,
                               const aegis_tool_args_t* args, long timeout_ms,
                               aegis_tool_result_t* out);

/* ── Executor integration ─────────────────────────────────────────────── */

/**
 * @brief Descriptor binding one pending task to one tool invocation.
 *
 * Owned by the bridge work function: created via aegis_tool_job_create(),
 * consumed (destroyed) after exactly one execution attempt, whether that
 * attempt succeeds, fails, times out, or is cancelled. If submit fails,
 * the job is destroyed before returning the error.
 */
typedef struct aegis_tool_job aegis_tool_job_t;

/**
 * @brief Create a job. Takes TRANSFERRED ownership of @p args (which must
 *        have been created by aegis_tool_args_create()).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_tool_job_create(aegis_tool_job_t** out, aegis_tool_registry_t* reg,
                                     const char* tool_name, aegis_tool_args_t* args);

/** Destroy a job, including its argument list. NULL-safe. */
void aegis_tool_job_destroy(aegis_tool_job_t* job);

/**
 * @brief Bridge work function matching aegis_work_fn.
 *
 * Resolves and executes the job's tool, then writes the result into the
 * task output as canonical bytes:
 *
 *     BOOL  -> 1 byte  (0 or 1)
 *     INT   -> 8 bytes little-endian two's complement
 *     FLOAT -> 8 bytes IEEE-754, host order (memcpy of double)
 *     STRING-> len bytes without the NUL terminator
 *     BYTES -> len bytes verbatim
 *
 * Failures set a task error message ("tool '<name>': <status>") and
 * propagate the tool/validation status verbatim so the executor's
 * normal classification (FAILED / TIMED_OUT / CANCELLED) applies.
 * Consumes (destroys) the job (user pointer) in all cases.
 */
extern aegis_work_fn aegis_tool_work_fn;

/**
 * @brief Submit a task bound to a tool invocation.
 *
 * Equivalent to aegis_executor_submit(exec, task, aegis_tool_work_fn, job)
 * with job built from (@p reg, @p tool_name, @p args). On submit failure
 * the transferred argument list is destroyed and the error propagates.
 *
 * Task timeout governs deadline arming (set it via
 * aegis_task_set_timeout_ms() before submitting).
 *
 * @return AEGIS_OK or any aegis_executor_submit()/job-creation error.
 */
aegis_status_t aegis_tool_submit(aegis_executor_t* exec, aegis_tool_registry_t* reg,
                                 aegis_task_t* task, const char* tool_name,
                                 aegis_tool_args_t* args);

#endif /* AEGIS_TOOL_H */
