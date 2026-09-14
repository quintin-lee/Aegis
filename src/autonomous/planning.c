/**
 * @file planning.c
 * @brief Plan phase: dispatches the goal through the planner into
 * runtime->plan. Rejects empty goals and missing planner up front.
 */
#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <string.h>

/**
 * @brief Run the plan phase: turn the goal into runtime->plan via the planner.
 *
 * Rejects NULL/empty inputs and a missing planner, returns CANCELLED when the
 * effective token is already set, and keeps an existing plan untouched (the
 * replan path replaces it via the caller). On success the plan is stored,
 * plans_generated is bumped, and an empty runtime goal is seeded.
 *
 * @param[in] agent    Agent carrying the planner; must be non-NULL.
 * @param[in] runtime  Runtime receiving the new plan; must be non-NULL.
 * @param[in] goal     Non-empty goal text; must be non-NULL.
 *
 * @return AEGIS_OK with runtime->plan set; INVALID/CANCELLED/INTERNAL or the
 *         planner's error otherwise.
 */
aegis_status_t aegis_autonomous_plan(aegis_autonomous_agent_t*   agent,
                                     aegis_autonomous_runtime_t* runtime, const char* goal)
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

    aegis_cancellation_token_t* token = aegis_autonomous_get_token(agent);
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
