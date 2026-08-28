/**
 * @file replanner.h
 * @brief Failure-driven replanning: old plan + feedback -> new plan version.
 *
 * Replanning is the planner's answer to execution failure. It re-prompts
 * the same LLM with the serialized previous plan and a free-form feedback
 * section (typically produced by reflection), then parses and validates
 * the response like any other plan.
 *
 * Versioning contract: a successfully replanned plan is stamped with
 * previous_version + 1, so consumers can detect stale plans.
 *
 * The replanner executes nothing and touches nothing but plan objects.
 */
#ifndef AEGIS_REPLANNER_H
#define AEGIS_REPLANNER_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/planner/planner.h"
#include "aegis/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Produce a revised plan for a goal that failed or needs adjustment.
 *
 * @param planner  Planner to use (borrowed).
 * @param old_plan Previous plan (borrowed; serialized into the prompt).
 * @param feedback Free-form failure/adjustment description (required non-empty).
 * @param token    Cancellation token (borrowed; may be NULL where permitted).
 * @param[out] out Receives the new plan (version = old version + 1).
 *                Ownership: transferred. Untouched on error.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL args / unparsable response),
 *         AEGIS_ERR_NOMEM, or any provider-dispatch status.
 */
aegis_status_t aegis_replan(const aegis_planner_t* planner, const aegis_plan_t* old_plan,
                            const char* feedback, const aegis_cancellation_token_t* token,
                            aegis_plan_t** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_REPLANNER_H */
