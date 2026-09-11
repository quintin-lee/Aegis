/**
 * @file lifecycle.c
 * @brief Agent lifecycle helpers: effective-token resolution (explicit
 * config token wins, else the owned internal token) plus init/cleanup
 * for planner, scheduler, executor and owned policy objects.
 */
#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Resolve the effective cancellation token for an agent.
 *
 * An explicit config token wins; otherwise the agent-owned internal token is
 * used. All public entry points funnel through here.
 *
 * @param[in] aa  Agent instance; NULL yields NULL.
 *
 * @return Borrowed token pointer, or NULL when the agent carries none.
 */
aegis_cancellation_token_t* aegis_autonomous_get_token(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return NULL;
    }
    if (aa->cfg.cancel_token) {
        return aa->cfg.cancel_token;
    }
    return aa->owned_token;
}

/**
 * @brief Allocate a zeroed per-run runtime context.
 *
 * Counters (iteration, checkpoint sequence, task stats, plan/replan counts)
 * and the recovering flag all start cleared; plan/graph/reflection slots
 * start NULL and are filled by the loop phases.
 *
 * @param[out] out  Receives the new runtime on success; untouched on failure.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID/NOMEM otherwise.
 *
 * Ownership: caller owns *out and must call
 * aegis_autonomous_runtime_destroy().
 */
aegis_status_t aegis_autonomous_runtime_create(aegis_autonomous_runtime_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_autonomous_runtime_t* rt = calloc(1, sizeof(*rt));
    if (!rt) {
        return AEGIS_ERR_NOMEM;
    }
    rt->iteration           = 0;
    rt->checkpoint_sequence = 0;
    rt->tasks_executed      = 0;
    rt->tasks_failed        = 0;
    rt->tasks_retried       = 0;
    rt->plans_generated     = 0;
    rt->replans             = 0;
    rt->recovering          = false;
    *out                    = rt;
    return AEGIS_OK;
}

/**
 * @brief Destroy a runtime context and the plan/graph/reflection it holds.
 *
 * NULL is a no-op. The last critique is a plain struct (no cleanup needed);
 * replan_feedback is freed. Must not be called while a loop uses the runtime.
 *
 * @param[in] rt  Runtime to destroy; NULL is accepted.
 */
void aegis_autonomous_runtime_destroy(aegis_autonomous_runtime_t* rt)
{
    if (!rt) {
        return;
    }
    if (rt->plan) {
        aegis_plan_destroy(rt->plan);
    }
    if (rt->graph) {
        aegis_task_graph_destroy(rt->graph);
    }
    if (rt->last_reflection) {
        aegis_reflection_destroy(rt->last_reflection);
    }
    free(rt->replan_feedback);
    free(rt);
}

/**
 * @brief Reset a runtime for a fresh goal without freeing the runtime itself.
 *
 * Destroys the current plan, task graph and last reflection, frees pending
 * replan feedback, zeroes the last critique and clears the recovering flag,
 * so the same runtime struct can back the next run. Counters are preserved.
 * NULL is a no-op.
 *
 * @param[in] rt  Runtime to reset; NULL is accepted.
 */
void aegis_autonomous_runtime_reset(aegis_autonomous_runtime_t* rt)
{
    if (!rt) {
        return;
    }
    if (rt->plan) {
        aegis_plan_destroy(rt->plan);
        rt->plan = NULL;
    }
    if (rt->graph) {
        aegis_task_graph_destroy(rt->graph);
        rt->graph = NULL;
    }
    if (rt->last_reflection) {
        aegis_reflection_destroy(rt->last_reflection);
        rt->last_reflection = NULL;
    }
    free(rt->replan_feedback);
    rt->replan_feedback = NULL;
    memset(&rt->last_critique, 0, sizeof(rt->last_critique));
    rt->recovering = false;
}

/**
 * @brief Initialize submodule resources for an autonomous agent.
 *
 * Currently a reserved hook: construction already wires planner, scheduler,
 * executor and critic, so this is a no-op returning success.
 *
 * @param[in] aa  Agent instance; currently unused.
 *
 * @return AEGIS_OK always.
 */
aegis_status_t aegis_autonomous_lifecycle_init(aegis_autonomous_agent_t* aa)
{
    (void)aa;
    return AEGIS_OK;
}

/**
 * @brief Release submodule resources held by an autonomous agent.
 *
 * Reserved counterpart to lifecycle_init; currently a no-op because
 * aegis_autonomous_agent_destroy() tears down the submodules directly.
 *
 * @param[in] aa  Agent instance; currently unused.
 */
void aegis_autonomous_lifecycle_cleanup(aegis_autonomous_agent_t* aa)
{
    (void)aa;
}
