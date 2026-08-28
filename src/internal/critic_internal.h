/**
 * @file critic_internal.h
 * @brief Internal layout of the critic module.
 *
 * NOT part of the public ABI.
 */
#ifndef AEGIS_CRITIC_INTERNAL_H
#define AEGIS_CRITIC_INTERNAL_H

#include "aegis/critic/critic.h"
#include "aegis/task/task.h"

#include <stddef.h>

/** Maximum feedback text length (includes NUL terminator). */
#define CRITIC_FEEDBACK_MAX 512u

/** Built-in heuristic evaluator context. */
struct aegis_critic {
    char feedback[CRITIC_FEEDBACK_MAX]; /**< Last evaluation feedback text. */
};

/**
 * Built-in heuristic evaluator.
 *
 * Rules:
 *   INVALID  : graph is NULL, task count is 0, or goal is empty.
 *   SUCCESS  : every terminal task is SUCCESS.
 *   PARTIAL  : at least one FAILED task exists, but at least one SUCCESS task
 *              also exists (partial progress was made).
 *   FAILURE  : all terminal tasks are FAILED with no SUCCESS task.
 *   REPLAN_REQUIRED: terminal state reached (all tasks terminal) but outcome
 *                    unsatisfactory; or cancellation/skipped tasks exist while
 *                    other tasks remain non-terminal.
 */
aegis_status_t aegis_critic_evaluate_builtin(aegis_critic_t* critic, const char* goal,
                                             const aegis_plan_t*               plan,
                                             const aegis_task_graph_t*         graph,
                                             const aegis_cancellation_token_t* token,
                                             aegis_critique_t*                 out);

#endif /* AEGIS_CRITIC_INTERNAL_H */
