/**
 * @file reflection.c
 * @brief Task-graph outcome summarization for the replanning loop.
 */
#include "aegis/reflection/reflection.h"

#include "aegis/task/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REFLECTION_ERROR_MAX    256u
#define REFLECTION_FEEDBACK_MAX 512u

struct aegis_reflection {
    size_t success;
    size_t failed;
    size_t cancelled;
    size_t skipped;
    size_t incomplete;

    char first_error[REFLECTION_ERROR_MAX]; /**< Empty when none. */
    char feedback[REFLECTION_FEEDBACK_MAX]; /**< Always populated. */
};

static void record_failure(struct aegis_reflection* r, const aegis_task_t* task)
{
    if (r->first_error[0] != '\0') {
        return; /* Keep the FIRST failure only. */
    }
    const char* err = aegis_task_error(task);
    if (!err || err[0] == '\0') {
        return;
    }
    snprintf(r->first_error, sizeof(r->first_error), "%s", err);
}

/**
 * @brief Build a reflection from the completed task graph: count outcomes,
 *        capture the first failure (if any), and synthesize a short
 *        feedback string for the replanner.
 *
 * Ownership of the resulting reflection is transferred to the caller.
 * The graph is queried only (no mutation). The feedback is always
 * populated — even on all-success it contains a summary line.
 *
 * @param[out] out  Receives the new reflection; untouched on failure.
 * @param[in]  graph Task graph to summarise (must be non-NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure, else the graph query error.
 */
aegis_status_t aegis_reflection_create(aegis_reflection_t** out, const aegis_task_graph_t* graph)
{
    if (!out || !graph) {
        return AEGIS_ERR_INVALID;
    }

    aegis_reflection_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_task_t** tasks = NULL;
    size_t         count = 0;
    aegis_status_t rc    = aegis_task_graph_tasks(graph, &tasks, &count);
    if (rc != AEGIS_OK) {
        free(r);
        return rc;
    }

    const char* first_failed_name = NULL;
    for (size_t i = 0; i < count; i++) {
        const aegis_task_t* t = tasks[i];
        if (!t) {
            continue;
        }
        switch (aegis_task_state(t)) {
        case AEGIS_TASK_SUCCESS:
            r->success++;
            break;
        case AEGIS_TASK_FAILED:
            r->failed++;
            record_failure(r, t);
            if (!first_failed_name) {
                first_failed_name = aegis_task_name(t);
            }
            break;
        case AEGIS_TASK_CANCELLED:
            r->cancelled++;
            break;
        case AEGIS_TASK_SKIPPED:
            r->skipped++;
            break;
        default: /* PENDING / READY / RUNNING / WAITING */
            r->incomplete++;
            break;
        }
    }
    free(tasks);

    int n = snprintf(r->feedback, sizeof(r->feedback),
                     "Execution summary: %zu succeeded, %zu failed, %zu cancelled, "
                     "%zu skipped, %zu incomplete.",
                     r->success, r->failed, r->cancelled, r->skipped, r->incomplete);
    if (n < 0) {
        free(r);
        return AEGIS_ERR_INTERNAL;
    }

    if (r->failed > 0 && first_failed_name && r->first_error[0] != '\0') {
        snprintf(r->feedback + n, sizeof(r->feedback) - (size_t)n,
                 " First failure in step '%s': %s", first_failed_name, r->first_error);
    }

    *out = r;
    return AEGIS_OK;
}

/**
 * @brief Free a reflection. NULL is a no-op.
 *
 * @param[in] refl Reflection to destroy, or NULL.
 */
void aegis_reflection_destroy(aegis_reflection_t* refl)
{
    free(refl);
}

/**
 * @brief Return the number of successful tasks.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Success count, or 0 for NULL.
 */
size_t aegis_reflection_success_count(const aegis_reflection_t* refl)
{
    return refl ? refl->success : 0;
}

/**
 * @brief Return the number of failed tasks.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Failed count, or 0 for NULL.
 */
size_t aegis_reflection_failed_count(const aegis_reflection_t* refl)
{
    return refl ? refl->failed : 0;
}

/**
 * @brief Return the number of cancelled tasks.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Cancelled count, or 0 for NULL.
 */
size_t aegis_reflection_cancelled_count(const aegis_reflection_t* refl)
{
    return refl ? refl->cancelled : 0;
}

/**
 * @brief Return the number of skipped tasks.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Skipped count, or 0 for NULL.
 */
size_t aegis_reflection_skipped_count(const aegis_reflection_t* refl)
{
    return refl ? refl->skipped : 0;
}

/**
 * @brief Return the number of incomplete (non-terminal) tasks.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Incomplete count, or 0 for NULL.
 */
size_t aegis_reflection_incomplete_count(const aegis_reflection_t* refl)
{
    return refl ? refl->incomplete : 0;
}

/**
 * @brief Return the first captured task error message, if any.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Borrowed NUL-terminated error string, or NULL when no failure
 *   was recorded (or when @p refl is NULL).
 */
const char* aegis_reflection_first_error(const aegis_reflection_t* refl)
{
    if (!refl || refl->first_error[0] == '\0') {
        return NULL;
    }
    return refl->first_error;
}

/**
 * @brief Return the synthesized feedback string for the replanner.
 *
 * @param[in] refl Reflection, or NULL.
 * @return Borrowed NUL-terminated feedback; always non-NULL when @p
 *   refl is non-NULL (returns an empty string for NULL).
 */
const char* aegis_reflection_feedback(const aegis_reflection_t* refl)
{
    return refl ? refl->feedback : "";
}
