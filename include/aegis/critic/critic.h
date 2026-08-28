/**
 * @file critic.h
 * @brief Plan-and-graph evaluation: determine whether execution achieved the goal.
 *
 * The Critic inspects a Goal, a Plan, and the resulting Task Graph after
 * execution (or mid-flight snapshot) and returns a structured evaluation.
 *
 * Evaluation outcomes:
 *   SUCCESS           all terminal tasks succeeded and the plan is fulfilled
 *   PARTIAL           some tasks failed or did not complete; the plan was only
 *                     partly achieved — sufficient information may remain to
 *                     continue without a full replan
 *   FAILURE           terminal tasks failed and there is no recoverable state
 *   INVALID           structural problem (plan/graph mismatch, missing goal,
 *                     empty graph) that makes any semantic evaluation impossible
 *   REPLAN_REQUIRED   execution reached terminal state but the outcome was
 *                     unsatisfactory; a fresh plan is needed
 *
 * Built-in behaviour (when no custom evaluator is supplied):
 *   - INVALID : graph is NULL, task count is 0, or goal is empty
 *   - SUCCESS : every task in the graph is AEGIS_TASK_SUCCESS
 *   - PARTIAL : at least one FAILED task exists, but at least one task also
 *               succeeded (something was accomplished)
 *   - FAILURE : all terminal tasks are FAILED with no successful task
 *   - REPLAN_REQUIRED : any terminal task was cancelled or skipped while
 *                       another task remains non-terminal (incomplete work
 *                       suggests the plan needs restructuring)
 *
 * Custom evaluators receive the same inputs and may inspect task outputs
 * (via aegis_task_output / aegis_task_error) when richer heuristics are needed.
 *
 * Thread safety: the Critic instance is read-only after creation; concurrent
 * evaluate() calls on different instances are safe. Callers synchronising a
 * single instance must do so externally.
 */
#ifndef AEGIS_CRITIC_H
#define AEGIS_CRITIC_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/planner/plan.h"
#include "aegis/status.h"
#include "aegis/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version of the critic interface. Bump on breaking changes. */
#define AEGIS_CRITIC_ABI_VERSION 1u

/**
 * @brief Evaluation outcome.
 *
 * Consumers map these to the next action in the agent loop:
 *   SUCCESS         → terminate with success
 *   PARTIAL         → optionally feed feedback to replan for missing steps
 *   FAILURE         → terminate with failure
 *   INVALID         → bug in caller; plan/graph/goal contract violated
 *   REPLAN_REQUIRED → call aegis_replan() with the feedback string
 */
typedef enum aegis_critique_result {
    AEGIS_CRITIQUE_INVALID,         /**< Structural contract violation.           */
    AEGIS_CRITIQUE_SUCCESS,         /**< Goal fully achieved.                      */
    AEGIS_CRITIQUE_PARTIAL,         /**< Partially achieved; some progress made.   */
    AEGIS_CRITIQUE_FAILURE,         /**< No recoverable progress.                  */
    AEGIS_CRITIQUE_REPLAN_REQUIRED, /**< Outcome unsatisfactory; replan needed.    */
} aegis_critique_result_t;

/**
 * @brief Result of a single critic evaluation.
 *
 * @c feedback is a human-readable explanation. Ownership: borrowed from the
 * returned critique instance (valid until destroy).
 */
typedef struct aegis_critique {
    aegis_critique_result_t result;   /**< Evaluation outcome. */
    const char*             feedback; /**< Explanation (may be empty). */
} aegis_critique_t;

/* ── Built-in evaluator context (no-op) ───────────────────────────────────── */

/** Opaque handle for the default evaluator. */
typedef struct aegis_critic aegis_critic_t;

/**
 * @brief Create a Critic using the built-in heuristic evaluator.
 *
 * @param[out] out  Receives the critic. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_critic_create(aegis_critic_t** out);

/**
 * @brief Evaluate a goal/plan/graph triplet.
 *
 * @param critic   Critic instance (borrowed).
 * @param goal     Goal text (borrowed; may be empty — yields INVALID).
 * @param plan     Plan that was executed (borrowed; may be NULL).
 * @param graph    Executed task graph (borrowed; may be NULL — yields INVALID).
 * @param token    Cancellation token (borrowed; may be NULL).
 * @param[out] out Receives the critique. Ownership: stays with the Critic
 *                 instance; valid until destroy.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL args), or AEGIS_ERR_CANCELLED.
 */
aegis_status_t aegis_critic_evaluate(const aegis_critic_t* critic, const char* goal,
                                     const aegis_plan_t* plan, const aegis_task_graph_t* graph,
                                     const aegis_cancellation_token_t* token,
                                     aegis_critique_t*                 out);

/** Destroy a Critic. Safe to call with NULL. */
void aegis_critic_destroy(aegis_critic_t* critic);

/** Return the static string name of a critique result. */
const char* aegis_critique_result_str(aegis_critique_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CRITIC_H */
