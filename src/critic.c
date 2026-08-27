/**
 * @file critic.c
 * @brief Plan-and-graph evaluation: determine whether execution achieved the goal.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/critic.h"
#include "aegis/task.h"

#include "internal/critic_internal.h"
#include "internal/lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal evaluation ───────────────────────────────────────────────────── */

aegis_status_t aegis_critic_evaluate_builtin(aegis_critic_t* critic, const char* goal,
                                             const aegis_plan_t*               plan,
                                             const aegis_task_graph_t*         graph,
                                             const aegis_cancellation_token_t* token,
                                             aegis_critique_t*                 out)
{
    (void)plan;
    if (!critic || !out) {
        return AEGIS_ERR_INVALID;
    }

    /* Cancellation gate. */
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    /* Structural pre-checks. */
    if (!goal || goal[0] == '\0') {
        out->result   = AEGIS_CRITIQUE_INVALID;
        out->feedback = "";
        return AEGIS_OK;
    }
    if (!graph) {
        out->result   = AEGIS_CRITIQUE_INVALID;
        out->feedback = "Task graph is NULL — cannot evaluate without an executed graph.";
        return AEGIS_OK;
    }
    if (aegis_task_graph_task_count(graph) == 0) {
        out->result   = AEGIS_CRITIQUE_INVALID;
        out->feedback = "Task graph is empty — no tasks were executed.";
        return AEGIS_OK;
    }

    /* Count terminal states. */
    size_t      n_success       = 0;
    size_t      n_failed        = 0;
    size_t      n_cancelled     = 0;
    size_t      n_skipped       = 0;
    size_t      n_incomplete    = 0;
    const char* first_failure   = NULL;
    const char* first_fail_name = NULL;

    aegis_task_t** tasks = NULL;
    size_t         count = 0;
    aegis_status_t rc    = aegis_task_graph_tasks(graph, &tasks, &count);
    if (rc != AEGIS_OK) {
        out->result   = AEGIS_CRITIQUE_INVALID;
        out->feedback = "Failed to enumerate tasks from graph.";
        return AEGIS_OK;
    }

    for (size_t i = 0; i < count; i++) {
        const aegis_task_t* t = tasks[i];
        if (!t) {
            continue;
        }
        switch (aegis_task_state(t)) {
        case AEGIS_TASK_SUCCESS:
            n_success++;
            break;
        case AEGIS_TASK_FAILED:
            n_failed++;
            if (!first_fail_name) {
                first_fail_name = aegis_task_name(t);
                first_failure   = aegis_task_error(t);
            }
            break;
        case AEGIS_TASK_CANCELLED:
            n_cancelled++;
            break;
        case AEGIS_TASK_SKIPPED:
            n_skipped++;
            break;
        default: /* PENDING / READY / RUNNING / WAITING */
            n_incomplete++;
            break;
        }
    }
    free(tasks);

    /* Decide outcome. */
    if (n_incomplete > 0) {
        /* Execution still in flight or was interrupted mid-run. */
        if (n_cancelled > 0 || n_skipped > 0) {
            int n         = snprintf(critic->feedback, sizeof(critic->feedback),
                                     "Incomplete execution: %zu succeeded, %zu failed, "
                                     "%zu cancelled, %zu skipped, %zu still pending.",
                                     n_success, n_failed, n_cancelled, n_skipped, n_incomplete);
            out->feedback = (n < 0) ? "" : critic->feedback;
        } else {
            int n         = snprintf(critic->feedback, sizeof(critic->feedback),
                                     "Execution in progress: %zu succeeded, %zu failed, "
                                     "%zu still pending.",
                                     n_success, n_failed, n_incomplete);
            out->feedback = (n < 0) ? "" : critic->feedback;
        }
        out->result = AEGIS_CRITIQUE_REPLAN_REQUIRED;
        return AEGIS_OK;
    }

    /* All tasks terminal. */
    if (n_failed == 0 && n_cancelled == 0 && n_skipped == 0) {
        out->result   = AEGIS_CRITIQUE_SUCCESS;
        out->feedback = "All tasks succeeded.";
        return AEGIS_OK;
    }

    if (n_success > 0) {
        /* Partial progress: some work completed, some did not.
         * Cancelled or skipped tasks signal an interrupted execution — replan needed. */
        if (n_cancelled > 0 || n_skipped > 0) {
            int n = snprintf(critic->feedback, sizeof(critic->feedback),
                             "Execution interrupted: %zu succeeded, %zu cancelled, "
                             "%zu skipped. A new plan is needed.",
                             n_success, n_cancelled, n_skipped);
            out->feedback = (n < 0) ? "" : critic->feedback;
            out->result   = AEGIS_CRITIQUE_REPLAN_REQUIRED;
            return AEGIS_OK;
        }
        int n = snprintf(critic->feedback, sizeof(critic->feedback),
                         "Partial success: %zu/%zu tasks succeeded; "
                         "%zu failed, %zu cancelled, %zu skipped.",
                         n_success, n_success + n_failed + n_cancelled + n_skipped, n_failed,
                         n_cancelled, n_skipped);
        out->feedback = (n < 0) ? "" : critic->feedback;
        out->result   = AEGIS_CRITIQUE_PARTIAL;
        return AEGIS_OK;
    }

    /* Total failure. */
    int n         = snprintf(critic->feedback, sizeof(critic->feedback),
                             "Failure: all %zu tasks failed/cancelled/skipped. "
                             "First failure in step '%s': %s",
                             n_failed + n_cancelled + n_skipped,
                             first_fail_name ? first_fail_name : "(unknown)",
                             first_failure ? first_failure : "(no message)");
    out->feedback = (n < 0) ? "" : critic->feedback;
    out->result   = AEGIS_CRITIQUE_FAILURE;
    return AEGIS_OK;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

aegis_status_t aegis_critic_create(aegis_critic_t** out)
{
    AEGIS_CHECK_OUT(out);
    *out = calloc(1, sizeof(**out));
    if (!*out) {
        return AEGIS_ERR_NOMEM;
    }
    (*out)->feedback[0] = '\0';
    return AEGIS_OK;
}

aegis_status_t aegis_critic_evaluate(const aegis_critic_t* critic, const char* goal,
                                     const aegis_plan_t* plan, const aegis_task_graph_t* graph,
                                     const aegis_cancellation_token_t* token, aegis_critique_t* out)
{
    return aegis_critic_evaluate_builtin((aegis_critic_t*)critic, goal, plan, graph, token, out);
}

void aegis_critic_destroy(aegis_critic_t* critic)
{
    free(critic);
}

const char* aegis_critique_result_str(aegis_critique_result_t result)
{
    switch (result) {
    case AEGIS_CRITIQUE_INVALID:
        return "INVALID";
    case AEGIS_CRITIQUE_SUCCESS:
        return "SUCCESS";
    case AEGIS_CRITIQUE_PARTIAL:
        return "PARTIAL";
    case AEGIS_CRITIQUE_FAILURE:
        return "FAILURE";
    case AEGIS_CRITIQUE_REPLAN_REQUIRED:
        return "REPLAN_REQUIRED";
    default:
        return "UNKNOWN";
    }
}
