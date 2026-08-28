/**
 * @file planner.h
 * @brief Goal -> Plan planning front end backed by an LLM provider.
 *
 * The Planner turns a natural-language goal into a structured, versioned
 * @c aegis_plan_t. It NEVER executes anything: no tools are invoked, no
 * file/network/process access happens here. Its sole side effect is the
 * produced plan object.
 *
 * The LLM is reached exclusively through the Provider Registry
 * abstraction (provider/llm.h); the planner holds a borrowed registry
 * pointer plus the name of the registered LLM provider to use.
 *
 * The expected model output is a strict line DSL (one step per line):
 *   STEP|<step_id>|<type>|<deps>|<name>|<description>
 * Anything else makes planning fail with AEGIS_ERR_INVALID rather than
 * producing a partially understood plan.
 *
 * Ownership: the planner owns its configuration copy (provider name).
 * Produced plans are transferred to the caller.
 *
 * Thread safety: a planner instance is single-threaded (documented
 * builder semantics); the underlying provider registry is thread-safe.
 */
#ifndef AEGIS_PLANNER_H
#define AEGIS_PLANNER_H

#include "aegis/executor/cancellation.h"
#include "aegis/planner/plan.h"
#include "aegis/provider/provider.h"
#include "aegis/status.h"
#include "aegis/strategy/strategy.h"
#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configuration for creating a planner instance (all borrowed). */
typedef struct aegis_planner_config {
    /** Provider registry used for LLM dispatch. Borrowed; must outlive the planner. */
    const aegis_provider_registry_t* provider_registry;
    /** Name of the registered LLM provider to plan with. Copied. */
    const char* llm_provider_name;
} aegis_planner_config_t;

/** Opaque planner handle (types.h forward-declares the tag). */
typedef struct aegis_planner aegis_planner_t;

/**
 * @brief Create a planner.
 *
 * @param[out] out  Receives the planner. Ownership: transferred.
 * @param[in]  cfg  Configuration (borrowed). Both fields required non-NULL.
 * @return AEGIS_OK, AEGIS_ERR_INVALID or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_planner_create(aegis_planner_t** out, const aegis_planner_config_t* cfg);

/** Destroy the planner. Safe to call with NULL. */
void aegis_planner_destroy(aegis_planner_t* planner);

/**
 * @brief Plan: turn a goal into a validated plan (version 1).
 *
 * Sends a structured prompt to the configured LLM provider, parses the
 * strict line-DSL response and validates the resulting plan before
 * returning it. On any parse/validation failure the function returns
 * AEGIS_ERR_INVALID and produces nothing (the caller cannot receive a
 * half-understood plan).
 *
 * Provider dispatch errors (unknown provider, uninitialized, cancelled
 * token, transport failures) propagate verbatim from aegis_llm_complete().
 *
 * @param planner Planner (borrowed).
 * @param goal    Goal text (borrowed, required non-empty).
 * @param token   Cancellation token (borrowed, may be NULL where the
 *                underlying dispatch permits it).
 * @param[out] out Receives the plan. Ownership: transferred. Untouched on error.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL args / unparsable or invalid
 *         plan), AEGIS_ERR_NOMEM, or any provider-dispatch status.
 */
aegis_status_t aegis_planner_plan(const aegis_planner_t* planner, const char* goal,
                                  const aegis_cancellation_token_t* token, aegis_plan_t** out);

/**
 * @brief Attach the strategy registry the planner resolves strategies in.
 *
 * Borrowed; must outlive every planning call that uses it. Attaching
 * does not change behavior until a strategy is selected via
 * aegis_planner_use_strategy().
 *
 * @return AEGIS_OK or AEGIS_ERR_INVALID (NULL args).
 */
aegis_status_t aegis_planner_attach_strategies(aegis_planner_t*                 planner,
                                               const aegis_strategy_registry_t* strategies);

/**
 * @brief Select the replaceable planning strategy by registry name.
 *
 * Once set, aegis_planner_plan() and aegis_replan() route through the
 * named strategy instead of the built-in DSL path. Pass NULL to return
 * to the built-in path.
 *
 * Resolution happens per call: an unknown name fails the planning
 * attempt with AEGIS_ERR_NOT_FOUND. Selecting a strategy without an
 * attached registry fails with AEGIS_ERR_INVALID.
 *
 * @param name Strategy name, or NULL for built-in (copied when non-NULL).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_planner_use_strategy(aegis_planner_t* planner, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PLANNER_H */
