/**
 * @file replanner.c
 * @brief Failure-driven replanning flow.
 *
 * Reuses the planner's LLM round-trip and strict parser; the only
 * replanning-specific logic is prompt shaping (previous plan + feedback)
 * and the version stamp.
 */
#include "internal/planner_internal.h"

#include "aegis/cancellation.h"
#include "aegis/plan.h"
#include "aegis/planner.h"
#include "aegis/replanner.h"
#include "aegis/strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_replan_intro[] =
    "The previous plan failed or must be adjusted. Produce a REVISED plan.\n"
    "Keep what works, fix what the feedback describes. Same line format.\n\n"
    "PREVIOUS PLAN:\n";

static const char k_feedback_header[] = "\nFEEDBACK:\n";

aegis_status_t aegis_replan(const aegis_planner_t* planner, const aegis_plan_t* old_plan,
                            const char* feedback, const aegis_cancellation_token_t* token,
                            aegis_plan_t** out)
{
    if (!planner || !old_plan || !feedback || feedback[0] == '\0' || !out) {
        return AEGIS_ERR_INVALID;
    }

    const char* goal = aegis_plan_goal(old_plan);

    /* Strategy-bound planners route the revision through their strategy.
     * Version stamping stays here: strategies only produce plans. */
    if (planner->strategy_name) {
        if (!planner->strategies) {
            return AEGIS_ERR_INVALID;
        }
        aegis_strategy_view_t view;
        aegis_status_t rc = aegis_strategy_find(planner->strategies, planner->strategy_name, &view);
        if (rc != AEGIS_OK) {
            return rc; /* NOT_FOUND propagates verbatim. */
        }
        aegis_strategy_input_t input = {goal, old_plan, feedback};
        rc                           = view.def.plan(view.def.user, &input, token, out);
        if (rc != AEGIS_OK) {
            return rc;
        }
        aegis_plan_set_version(*out, aegis_plan_version(old_plan) + 1u);
        return AEGIS_OK;
    }

    char*          serialized = NULL;
    aegis_status_t rc         = aegis_plan_serialize(old_plan, &serialized);
    if (rc != AEGIS_OK) {
        return rc;
    }

    size_t need   = sizeof(k_replan_intro) + strlen(serialized) + sizeof(k_feedback_header) +
                    strlen(feedback) + 8;
    char*  prompt = malloc(need);
    if (!prompt) {
        free(serialized);
        return AEGIS_ERR_NOMEM;
    }
    snprintf(prompt, need, "%s%s%s%s\n", k_replan_intro, serialized, k_feedback_header, feedback);
    free(serialized);

    /* New plan carries the same goal text as the old one. */
    rc =
        aegis_planner_generate(planner->registry, planner->provider_name, prompt, goal, token, out);
    free(prompt);
    if (rc != AEGIS_OK) {
        return rc;
    }

    aegis_plan_set_version(*out, aegis_plan_version(old_plan) + 1u);
    return AEGIS_OK;
}
