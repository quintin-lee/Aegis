#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <string.h>

aegis_status_t autonomous_plan(aegis_autonomous_agent_t* agent, aegis_autonomous_runtime_t* runtime,
                               const char* goal)
{
    if (!agent || !runtime || !goal) {
        return AEGIS_ERR_INVALID;
    }
    if (goal[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (!agent->planner) {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token = autonomous_get_token(agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    // If plan already exists (replan path), keep it — caller handles replacement.
    // Otherwise create new plan.
    if (runtime->plan) {
        return AEGIS_OK;
    }

    aegis_plan_t*  plan = NULL;
    aegis_status_t rc   = aegis_planner_plan(agent->planner, goal, token, &plan);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (!plan) {
        return AEGIS_ERR_INTERNAL;
    }

    runtime->plan = plan;
    runtime->plans_generated++;
    if (runtime->goal[0] == '\0') {
        strncpy(runtime->goal, goal, sizeof(runtime->goal) - 1);
    }
    return AEGIS_OK;
}
