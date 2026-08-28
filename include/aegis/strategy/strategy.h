/**
 * @file strategy.h
 * @brief Replaceable planning strategies and their registry.
 *
 * A strategy is a named policy for producing or revising a structured
 * @c aegis_plan_t. Strategies are strictly plan-level components:
 *
 * - they NEVER execute tools, shell commands, or any side-effectful step;
 * - they never touch files, network sockets, or processes directly;
 * - LLM access (when needed) goes exclusively through the Provider
 *   Registry abstraction (provider/llm.h);
 * - their only output is a validated-or-failed plan object.
 *
 * The Planner depends on this interface only - it is never bound to a
 * concrete strategy. Concrete strategies live outside the core (see
 * @c src/providers/strategies/), mirroring how concrete providers do.
 *
 * ABI stability: like providers, strategies carry an explicit
 * @c abi_version that must match AEGIS_STRATEGY_ABI_VERSION at
 * registration time; mismatched definitions are rejected.
 *
 * Ownership: definition strings are borrowed and must outlive the
 * registration; the registry shallow-copies the def. Produced plans are
 * transferred to the caller.
 *
 * Thread safety: registries are thread-safe (leaf lock); individual
 * strategy callbacks declare their own contract via the def they ship -
 * the bundled plan_execute strategy is single-threaded by design.
 */
#ifndef AEGIS_STRATEGY_H
#define AEGIS_STRATEGY_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/planner/plan.h"
#include "aegis/status.h"
#include "aegis/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version of the strategy interface. Bump on breaking changes. */
#define AEGIS_STRATEGY_ABI_VERSION 1u

/**
 * @brief Inputs handed to a strategy for one planning attempt.
 *
 * A fresh planning request sets only @p goal. A revision request
 * (replan) additionally carries the previous plan and the feedback text
 * describing why it must change. All pointers are borrowed for the
 * duration of the call.
 */
typedef struct aegis_strategy_input {
    /** Goal text (required, non-empty). Borrowed. */
    const char* goal;
    /** Previous plan to revise, or NULL for fresh planning. Borrowed. */
    const aegis_plan_t* previous_plan;
    /** Feedback for the revision (required non-empty when previous_plan
     *  is set). Borrowed. */
    const char* feedback;
} aegis_strategy_input_t;

/**
 * @brief Strategy entry point: produce one plan from the inputs.
 *
 * Runs without registry or planner locks held. Implementations must
 * honor @p token cooperatively (return AEGIS_ERR_CANCELLED when it is
 * observed cancelled before/at dispatch).
 *
 * On success @p out receives a validated plan (ownership: transferred).
 * On failure *out is untouched and no partial plan escapes: return
 * AEGIS_ERR_INVALID for unparsable/invalid model output, or propagate
 * provider-dispatch statuses verbatim.
 *
 * @param user  The def's user pointer (borrowed).
 */
typedef aegis_status_t (*aegis_strategy_plan_fn)(void* user, const aegis_strategy_input_t* input,
                                                 const aegis_cancellation_token_t* token,
                                                 aegis_plan_t**                    out);

/**
 * @brief Strategy definition (registration input).
 *
 * The registry shallow-copies this struct. Keep name/description/user
 * valid for the lifetime of the registration.
 */
typedef struct aegis_strategy_def {
    /** Unique key (required, non-empty). Borrowed. */
    const char* name;
    /** Human-readable text (may be NULL). Borrowed. */
    const char* description;
    /** Must equal AEGIS_STRATEGY_ABI_VERSION. */
    uint32_t abi_version;
    /** Borrowed callback context (strategy-private). */
    void* user;
    /** Plan producer (required, non-NULL). */
    aegis_strategy_plan_fn plan;
} aegis_strategy_def_t;

/** Copied definition returned by lookup (value semantics; no interior pointers). */
typedef struct aegis_strategy_view {
    aegis_strategy_def_t def; /**< Shallow copy of the stored definition. */
} aegis_strategy_view_t;

/* ── Registry ─────────────────────────────────────────────────────────────── */

/** Opaque strategy registry handle (types.h forward-declares the tag). */
typedef struct aegis_strategy_registry aegis_strategy_registry_t;

/**
 * @brief Create an empty strategy registry.
 *
 * @param[out] out Receives the handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL out), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_strategy_registry_create(aegis_strategy_registry_t** out);

/**
 * @brief Destroy the registry and free all owned entries.
 *
 * Safe to call with NULL (no-op).
 */
void aegis_strategy_registry_destroy(aegis_strategy_registry_t* reg);

/**
 * @brief Register a strategy definition.
 *
 * @param reg Registry (borrowed).
 * @param def Definition to copy (borrowed). Validated: non-NULL fields,
 *            non-empty name, abi_version == AEGIS_STRATEGY_ABI_VERSION,
 *            plan callback non-NULL.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY (name taken),
 *         AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_strategy_register(aegis_strategy_registry_t*  reg,
                                       const aegis_strategy_def_t* def);

/**
 * @brief Look up a strategy by name.
 *
 * @param[out] view Receives a copy of the stored definition (borrowed
 *                  output buffer, required).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_strategy_find(const aegis_strategy_registry_t* reg, const char* name,
                                   aegis_strategy_view_t* view);

/**
 * @brief Unregister a strategy. Re-registering the same name afterwards
 *        is legal.
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_strategy_unregister(aegis_strategy_registry_t* reg, const char* name);

/** Number of registered strategies (NULL reg → 0). */
size_t aegis_strategy_count(const aegis_strategy_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRATEGY_H */
