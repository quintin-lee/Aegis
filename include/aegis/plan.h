/**
 * @file plan.h
 * @brief Versioned, structured, verifiable execution plan.
 *
 * A Plan is the Planner's ONLY output artifact: a structured description
 * of work (steps + dependencies) that can be
 *   - validated for structural integrity (aegis_plan_validate), and
 *   - materialized into a Task Graph (aegis_plan_materialize).
 *
 * A plan never executes anything. It holds no tool handles, no provider
 * handles, and performs no I/O. Steps may NAME a tool (a plain string);
 * binding that name to an actual tool happens exclusively at execution
 * time via the Tool Registry.
 *
 * Ownership:
 *   - The plan owns copies of every string and byte blob added to it.
 *   - aegis_plan_materialize() transfers a freshly created task graph to
 *     the caller; the plan itself is untouched and remains reusable.
 *
 * Thread safety: plans are single-threaded builder objects. Callers who
 * share a plan across threads must synchronize externally.
 */
#ifndef AEGIS_PLAN_H
#define AEGIS_PLAN_H

#include "aegis/graph.h"
#include "aegis/status.h"
#include "aegis/task.h"
#include "aegis/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of steps in one plan (hard bound against runaway LLM output). */
#define AEGIS_PLAN_MAX_STEPS 256u

/** Maximum dependencies per step. */
#define AEGIS_PLAN_MAX_DEPS 32u

/** Sentinel step id: assign the next free sequential id. */
#define AEGIS_PLAN_STEP_ID_AUTO (-1)

/**
 * @brief One unit of planned work.
 *
 * All strings/blobs are copied into the plan on add. Zero the struct and
 * fill only the fields you need; unset optional fields stay NULL/zero.
 */
typedef struct aegis_plan_step_spec {
    int64_t           step_id;       /**< AEGIS_PLAN_STEP_ID_AUTO or explicit unique id >= 0. */
    const char*       name;          /**< Required, non-empty. Borrowed.                       */
    const char*       desc;          /**< Optional. Borrowed.                                  */
    aegis_task_type_t type;          /**< Task type for materialization.                       */
    int               priority;      /**< Passed through to the materialized task.             */
    const char*       tool_name;     /**< Optional tool hint (string only, never resolved).    */
    const void*       input;         /**< Optional input bytes. Borrowed.                      */
    size_t            input_len;     /**< Input length in bytes.                               */
    long              timeout_ms;    /**< 0 = no timeout.                                      */
    aegis_task_retry_policy_t retry; /**< Retry policy for the materialized task.      */
    const int64_t*            deps;  /**< Dependency step ids (borrowed; must already exist).  */
    size_t dep_count;                /**< Number of entries in @c deps.                        */
} aegis_plan_step_spec_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty plan for a goal. Version starts at 1.
 *
 * @param[out] out  Receives the plan. Ownership: transferred.
 * @param[in]  goal Goal text (required, non-empty). Copied.
 */
aegis_status_t aegis_plan_create(aegis_plan_t** out, const char* goal);

/** Destroy the plan. Safe to call with NULL. */
void aegis_plan_destroy(aegis_plan_t* plan);

/* ── Accessors ─────────────────────────────────────────────────────────────── */

/** Goal text (borrowed; NULL-safe, NULL plan -> NULL). */
const char* aegis_plan_goal(const aegis_plan_t* plan);

/** Plan version. Starts at 1; replanning flows stamp higher versions. */
uint32_t aegis_plan_version(const aegis_plan_t* plan);

/**
 * @brief Stamp a plan version.
 *
 * Intended for replanning flows that must guarantee strictly increasing
 * versions across revisions of the same goal.
 */
void aegis_plan_set_version(aegis_plan_t* plan, uint32_t version);

/** Number of steps. NULL plan -> 0. */
size_t aegis_plan_step_count(const aegis_plan_t* plan);

/**
 * @brief Dependency count of one step (by id).
 * @return Dependency count, or 0 if the id is unknown.
 */
size_t aegis_plan_step_dep_count(const aegis_plan_t* plan, int64_t step_id);

/* ── Construction ──────────────────────────────────────────────────────────── */

/**
 * @brief Add a step.
 *
 * Validation performed here: non-empty name; id uniqueness (explicit ids)
 * or auto-assignment; dependency ids must reference steps ALREADY present
 * (no forward references); self-dependency rejected; per-step/per-plan
 * capacity limits enforced.
 *
 * @param plan Plan (borrowed).
 * @param spec Step description (borrowed; contents copied).
 * @param[out] out_id Receives the final step id (may be NULL).
 * @return AEGIS_OK, AEGIS_ERR_INVALID (bad fields/unknown dep/self-dep),
 *         AEGIS_ERR_BUSY (duplicate id) or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_plan_add_step(aegis_plan_t* plan, const aegis_plan_step_spec_t* spec,
                                   int64_t* out_id);

/* ── Verification ──────────────────────────────────────────────────────────── */

/**
 * @brief Validate the whole plan structurally.
 *
 * Checks (in order):
 *   1. at least one step,
 *   2. every step has a non-empty name,
 *   3. every dependency references an existing step,
 *   4. no self dependencies, no duplicate dependencies,
 *   5. the dependency graph is acyclic (depth-first search).
 *
 * @return AEGIS_OK when valid; AEGIS_ERR_INVALID otherwise.
 */
aegis_status_t aegis_plan_validate(const aegis_plan_t* plan);

/* ── Materialization ───────────────────────────────────────────────────────── */

/**
 * @brief Materialize the plan into a NEW task graph.
 *
 * Creates one task per step (name/description/type/priority/input/
 * timeout/retry carried over; the tool_name hint, when present, is stored
 * as task metadata under key "tool") and wires all declared dependencies.
 * The resulting graph is validated before being handed out; on any error
 * the partially built graph is destroyed and nothing leaks.
 *
 * The plan is NOT modified and keeps no reference to the produced graph.
 *
 * @param plan       Plan (borrowed).
 * @param[out] out   Receives the graph. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (invalid plan / materialization
 *         mismatch) or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_plan_materialize(const aegis_plan_t* plan, aegis_task_graph_t** out);

/* ── Serialization ─────────────────────────────────────────────────────────── */

/**
 * @brief Serialize the plan to the canonical line DSL.
 *
 * Format (one step per line, '|' separated):
 *   PLAN|<version>
 *   STEP|<id>|<type>|<deps comma separated>|<name>|<description>
 *
 * The output is NUL-terminated; release it with free().
 *
 * @param[out] out_str Receives the malloc'd string. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_plan_serialize(const aegis_plan_t* plan, char** out_str);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PLAN_H */
