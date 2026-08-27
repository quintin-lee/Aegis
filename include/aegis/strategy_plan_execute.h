/**
 * @file strategy_plan_execute.h
 * @brief Plan-and-execute strategy factory.
 *
 * The plan_execute strategy implements the default Goal -> Plan -> Task
 * Graph flow: for a fresh goal it asks the configured LLM provider for a
 * STEP DSL program and parses it into a validated plan; when revising it
 * serializes the previous plan plus the feedback text into the prompt.
 *
 * Like every strategy it never executes tools and never touches files,
 * network, or processes - it only produces structured plans. LLM access
 * goes exclusively through aegis_llm_complete() on the borrowed provider
 * registry.
 *
 * @note The context must outlive every registration made from it: the
 *       registry stores def.user as a borrowed pointer.
 */
#ifndef AEGIS_STRATEGY_PLAN_EXECUTE_H
#define AEGIS_STRATEGY_PLAN_EXECUTE_H

#include "aegis/provider.h"
#include "aegis/strategy.h"
#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Construction parameters for the plan_execute strategy.
 */
typedef struct aegis_strategy_plan_execute_ctx {
    const aegis_provider_registry_t* provider_registry; /**< borrowed; LLM dispatch source */
    const char*                      llm_provider_name; /**< borrowed; registered LLM provider */
} aegis_strategy_plan_execute_ctx_t;

/**
 * @brief Fill a strategy definition bound to @p ctx.
 *
 * The definition is named "plan_execute". @p out receives a plain value
 * copy; the caller registers it with aegis_strategy_register(). No
 * allocation occurs and no ownership transfers - both @p ctx and the
 * strings inside it stay owned by the caller and must outlive the
 * registration.
 *
 * @param[in]  ctx  construction parameters (borrowed)
 * @param[out] out  definition to fill
 * @return AEGIS_OK, or AEGIS_ERR_INVALID when @p ctx or its fields are NULL/empty
 */
aegis_status_t aegis_strategy_plan_execute_def(const aegis_strategy_plan_execute_ctx_t* ctx,
                                               aegis_strategy_def_t*                    out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRATEGY_PLAN_EXECUTE_H */
