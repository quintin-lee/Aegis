#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <stdlib.h>

aegis_status_t autonomous_replan(aegis_autonomous_agent_t*   agent,
                                 aegis_autonomous_runtime_t* runtime)
{
    if (!agent || !runtime || !runtime->plan || !runtime->replan_feedback) {
        return AEGIS_ERR_INVALID;
    }
    if (!agent->planner) {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token = autonomous_get_token(agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    aegis_plan_t*  new_plan = NULL;
    aegis_status_t rc =
        aegis_replan(agent->planner, runtime->plan, runtime->replan_feedback, token, &new_plan);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (!new_plan) {
        return AEGIS_ERR_INTERNAL;
    }

    aegis_plan_destroy(runtime->plan);
    runtime->plan = new_plan;
    runtime->replans++;
    // Graph will be rebuilt on next execution iteration
    if (runtime->graph) {
        aegis_task_graph_destroy(runtime->graph);
        runtime->graph = NULL;
    }
    return AEGIS_OK;
}
