/**
 * @file tool_call.c
 * @brief Tool call runtime: resolve -> validate -> execute, plus the
 *        executor bridge (job + work function + submit).
 *
 * The bridge keeps the executor free of any concrete-tool dependency:
 * aegis_tool_submit() merely installs aegis_tool_work_fn as the task's
 * work function; the executor classifies outcomes exactly as it does for
 * any other work.
 */
#include "cancellation_internal.h"
#include "task_internal.h"
#include "aegis/executor/executor.h"
#include "tool_internal.h"

#include "aegis/common/time.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Core invocation ──────────────────────────────────────────────────── */

/**
 * @brief Look up a tool by name, validate its args, and dispatch the
 *        execute callback under an optional cancellation token.
 *
 * Returns AEGIS_ERR_INVALID when the tool is unknown or args fail the
 * schema check. On execute failure the result payload is zeroed to avoid
 * leaking allocations from a misbehaving tool.
 *
 * @param[in]  reg     Tool registry (must be non-NULL).
 * @param[in]  name    Tool name to invoke (must be non-NULL, non-empty).
 * @param[in]  args    Call-time arguments (may be NULL — interpreted as empty).
 * @param[in]  token   Cancellation point, or NULL to ignore.
 * @param[out] out     Receives the result payload; set even on error.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_CANCELLED when @p token is tripped, else the execute error.
 */
aegis_status_t aegis_tool_execute(aegis_tool_registry_t* reg, const char* name,
                                  const aegis_tool_args_t*          args,
                                  const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    if (!reg || !name || !out) {
        return AEGIS_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    aegis_tool_def_t def;
    aegis_status_t   st = aegis_tool_registry_find(reg, name, &def);
    if (st != AEGIS_OK) {
        return st;
    }

    st = aegis_tool_validate_args(&def.schema, args);
    if (st != AEGIS_OK) {
        return st;
    }

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    st = def.execute(def.user, args, token, out);
    if (st != AEGIS_OK) {
        /* Tool contract says failure leaves the result zeroed; enforce it
         * so no payload allocated by a misbehaving tool can leak. */
        aegis_tool_result_destroy(out);
        memset(out, 0, sizeof(*out));
    }
    return st;
}

/**
 * @brief Execute a tool with a hard timeout.
 *
 * Builds a one-shot cancellation token with the given millisecond deadline
 * and delegates to aegis_tool_execute. A timeout_ms of 0 is rejected as
 * invalid (it would otherwise produce an always-expired deadline). The
 * caller owns the result payload on success and must call
 * aegis_tool_result_destroy.
 *
 * @param[in]  reg        Tool registry.
 * @param[in]  name       Tool name (non-NULL, non-empty).
 * @param[in]  args       Call-time arguments (may be NULL).
 * @param[in]  timeout_ms Timeout in milliseconds (must be > 0).
 * @param[out] out        Receives the result payload.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args or timeout_ms==0,
 *   AEGIS_ERR_TIMEOUT on deadline expiry, else the execute error.
 */
aegis_status_t aegis_tool_call(aegis_tool_registry_t* reg, const char* name,
                               const aegis_tool_args_t* args, long timeout_ms,
                               aegis_tool_result_t* out)
{
    if (!out || timeout_ms == 0) {
        /* timeout_ms == 0 would mean an always-expired deadline; reject
         * rather than surprise callers. */
        return AEGIS_ERR_INVALID;
    }

    struct aegis_cancellation_token tok = {AEGIS_CANCEL_NONE, 0};
    if (timeout_ms > 0) {
        const int64_t now = (int64_t)aegis_mono_now();
        const int64_t ns  = (int64_t)timeout_ms * 1000000;
        tok.deadline_ns   = (ns > INT64_MAX - now) ? INT64_MAX : now + ns;
    }

    return aegis_tool_execute(reg, name, args, &tok, out);
}

/* ── Job lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Allocate a tool-execution job that bundles the registry, tool
 *        name and argument list into a single object for the executor.
 *
 * The job takes ownership of @p args (caller must not use it afterwards)
 * and copies @p tool_name into heap storage. Destroy with
 * aegis_tool_job_destroy when no longer needed.
 *
 * @param[out] out       Receives the new job; untouched on failure.
 * @param[in]  reg       Registry the tool belongs to (must be non-NULL).
 * @param[in]  tool_name Tool name (must be non-NULL, non-empty).
 * @param[in]  args      Arguments to pass (consumed; may be NULL → empty).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_job_create(aegis_tool_job_t** out, aegis_tool_registry_t* reg,
                                     const char* tool_name, aegis_tool_args_t* args)
{
    if (!out || !reg || !tool_name || tool_name[0] == '\0' || !args) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_tool_job_t* job = calloc(1, sizeof(*job));
    if (!job) {
        return AEGIS_ERR_NOMEM;
    }
    job->reg  = reg;
    job->args = args; /* Transferred. */

    const size_t n = strlen(tool_name) + 1;
    job->name      = malloc(n);
    if (!job->name) {
        aegis_tool_job_destroy(job);
        return AEGIS_ERR_NOMEM;
    }
    memcpy(job->name, tool_name, n);

    *out = job;
    return AEGIS_OK;
}

/**
 * @brief Free a tool-execution job and release the owned args and name.
 *
 * NULL is a no-op. Safe to call after the job has already been consumed
 * by the executor (the executor calls destroy once it is done with the job).
 *
 * @param[in] job Job to destroy, or NULL.
 */
void aegis_tool_job_destroy(aegis_tool_job_t* job)
{
    if (!job) {
        return;
    }
    free(job->name);
    aegis_tool_args_destroy(job->args);
    free(job);
}

/* ── Result -> task-output byte encoding ──────────────────────────────── */

static void store_le64(uint8_t* dst, uint64_t v)
{
    for (size_t i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}

static aegis_status_t encode_value(const aegis_tool_value_t* v, aegis_task_t* task)
{
    switch (v->type) {
    case AEGIS_TOOL_VAL_BOOL: {
        const uint8_t b = v->as.b ? 1u : 0u;
        return aegis_task_set_output(task, &b, 1);
    }
    case AEGIS_TOOL_VAL_INT: {
        uint8_t buf[8];
        store_le64(buf, (uint64_t)v->as.i);
        return aegis_task_set_output(task, buf, sizeof(buf));
    }
    case AEGIS_TOOL_VAL_FLOAT: {
        /* IEEE-754 double, host byte order (memcpy). */
        return aegis_task_set_output(task, &v->as.f, sizeof(v->as.f));
    }
    case AEGIS_TOOL_VAL_STRING:
        return aegis_task_set_output(task, v->as.str.ptr, v->as.str.len);
    case AEGIS_TOOL_VAL_BYTES:
        return aegis_task_set_output(task, v->as.bytes.ptr, v->as.bytes.len);
    default:
        return AEGIS_ERR_INTERNAL;
    }
}

/* ── Executor bridge ──────────────────────────────────────────────────── */

static aegis_status_t tool_work_impl(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                     void* user)
{
    aegis_tool_job_t* job = (aegis_tool_job_t*)user;
    if (!job || !task) {
        aegis_tool_job_destroy(job);
        return AEGIS_ERR_INVALID;
    }

    aegis_tool_result_t result;
    aegis_status_t      rc = aegis_tool_execute(job->reg, job->name, job->args, token, &result);

    if (rc == AEGIS_OK) {
        rc = encode_value(&result.value, task);
        if (rc != AEGIS_OK) {
            char msg[96];
            (void)snprintf(msg, sizeof(msg), "tool '%s': output encoding failed (%d)", job->name,
                           (int)rc);
            aegis_task_set_error(task, msg);
        }
    } else {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "tool '%s': execution failed (%d)", job->name, (int)rc);
        aegis_task_set_error(task, msg);
    }

    aegis_tool_result_destroy(&result);
    aegis_tool_job_destroy(job); /* Consumed after exactly one attempt. */
    return rc;
}

aegis_work_fn aegis_tool_work_fn = tool_work_impl;

/**
 * @brief Submit a tool execution as an async work item on the executor.
 *
 * The caller retains ownership of @p args (transferred into the job, then
 * destroyed there) and @p task (the executor manages lifecycle). Returns
 * AEGIS_ERR_INVALID when exec/reg/tool_name/args are NULL. If the submit
 * fails the job is destroyed immediately so nothing leaks.
 *
 * @param[in] exec     Executor to submit the work onto (must be non-NULL).
 * @param[in] reg      Tool registry (must be non-NULL).
 * @param[in] task     Task representing this execution (must be non-NULL).
 * @param[in] tool_name Name of the tool to invoke (must be non-NULL, non-empty).
 * @param[in] args     Call arguments (consumed by the job; may be NULL → empty).
 * @return AEGIS_OK on submission success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOMEM on allocation failure, else the executor-submit error.
 */
aegis_status_t aegis_tool_submit(aegis_executor_t* exec, aegis_tool_registry_t* reg,
                                 aegis_task_t* task, const char* tool_name, aegis_tool_args_t* args)
{
    aegis_tool_job_t* job = NULL;
    aegis_status_t    st  = aegis_tool_job_create(&job, reg, tool_name, args);
    if (st != AEGIS_OK) {
        aegis_tool_args_destroy(args);
        return st;
    }

    st = aegis_executor_submit(exec, task, aegis_tool_work_fn, job);
    if (st != AEGIS_OK) {
        aegis_tool_job_destroy(job); /* Also destroys the transferred args. */
    }
    return st;
}
