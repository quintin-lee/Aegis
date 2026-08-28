/**
 * @file plan_execute.c
 * @brief Plan-and-execute strategy implementation.
 *
 * Fresh goals and revisions share one rule: produce a validated plan or
 * fail. The strategy owns its prompting policy (deliberately duplicating
 * the section layout instead of sharing planner internals) but reuses the
 * shared STEP DSL machinery via aegis_planner_generate() so parsing and
 * validation semantics cannot drift between the built-in path and
 * strategies.
 */
#include "aegis/strategy/strategy_plan_execute.h"

#include "planner_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Prompting policy (owned by this strategy)                           */
/* ------------------------------------------------------------------ */

static const char k_fresh_intro[] =
    "You are a planning engine for an autonomous agent.\n"
    "Produce a step-by-step execution plan for the goal below.\n\n";

static const char k_revise_intro[] =
    "You are a planning engine revising a failed plan.\n"
    "The previous plan did not fully succeed. Produce a corrected,\n"
    "complete plan that still achieves the original goal.\n\n";

static const char k_previous_section[] = "PREVIOUS PLAN:\n";
static const char k_feedback_section[] = "FEEDBACK:\n";

/* ------------------------------------------------------------------ */
/* plan fn                                                             */
/* ------------------------------------------------------------------ */

/**
 * Build the prompt body for @p input, run generation, hand out the plan.
 *
 * Version stamping is intentionally NOT done here: version semantics have
 * a single owner (the caller - planner/replanner), so strategies can never
 * corrupt them.
 */
static aegis_status_t plan_execute_plan(void* user, const aegis_strategy_input_t* input,
                                        const aegis_cancellation_token_t* token, aegis_plan_t** out)
{
    if (!user || !input || !out) {
        return AEGIS_ERR_INVALID;
    }
    const aegis_strategy_plan_execute_ctx_t* ctx = user;

    char* prompt = NULL;
    if (input->previous_plan) {
        /* Revision: previous plan + feedback sections. */
        char*          serialized = NULL;
        aegis_status_t rc         = aegis_plan_serialize(input->previous_plan, &serialized);
        if (rc != AEGIS_OK) {
            return rc;
        }

        size_t cap = strlen(k_revise_intro) + strlen(k_previous_section) + strlen(serialized) + 2 +
                     strlen(k_feedback_section) + strlen(input->feedback) + 3;
        prompt     = malloc(cap);
        if (!prompt) {
            free(serialized);
            return AEGIS_ERR_NOMEM;
        }
        snprintf(prompt, cap, "%s%s%s\n%s%s\n", k_revise_intro, k_previous_section, serialized,
                 k_feedback_section, input->feedback);
        free(serialized);
    } else {
        /* Fresh goal. */
        size_t cap = strlen(k_fresh_intro) + strlen("GOAL:\n") + strlen(input->goal) + 2;
        prompt     = malloc(cap);
        if (!prompt) {
            return AEGIS_ERR_NOMEM;
        }
        snprintf(prompt, cap, "%sGOAL:\n%s\n", k_fresh_intro, input->goal);
    }

    aegis_status_t rc = aegis_planner_generate(ctx->provider_registry, ctx->llm_provider_name,
                                               prompt, input->goal, token, out);
    free(prompt);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Factory                                                             */
/* ------------------------------------------------------------------ */

aegis_status_t aegis_strategy_plan_execute_def(const aegis_strategy_plan_execute_ctx_t* ctx,
                                               aegis_strategy_def_t*                    out)
{
    if (!ctx || !out || !ctx->provider_registry || !ctx->llm_provider_name ||
        ctx->llm_provider_name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }

    static const aegis_strategy_def_t k_def = {
        .name = "plan_execute",
        .description =
            "Default plan-then-execute strategy: LLM produces a STEP DSL "
            "plan; revisions serialize the failed plan plus feedback.",
        .abi_version = AEGIS_STRATEGY_ABI_VERSION,
        /* Non-const: def.user is a plain void* (borrowed, registry never writes). */
        .user = NULL,
        .plan = plan_execute_plan,
    };

    *out      = k_def;
    out->user = (void*)ctx;
    return AEGIS_OK;
}
