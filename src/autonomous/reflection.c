/**
 * @file reflection.c
 * @brief Reflect phase: rebuilds the reflection object from the task
 * graph and extracts the replan feedback text consumed by replanning.
 */
#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Run the reflect phase: distill failure lessons for the replanner.
 *
 * Drops any prior reflection/feedback, rebuilds a reflection object from the
 * current task graph, and snapshots its feedback text (falling back to a
 * generic "plan failed, need revision" note when empty) into
 * runtime->replan_feedback for the replan phase to consume.
 *
 * @param[in] agent    Agent instance; must be non-NULL.
 * @param[in] runtime  Runtime with a graph; receives reflection + feedback.
 *
 * @return AEGIS_OK with last_reflection/replan_feedback set; INVALID for
 *         missing inputs, NOMEM when the feedback copy fails.
 */
aegis_status_t aegis_autonomous_reflect(aegis_autonomous_agent_t*   agent,
                                        aegis_autonomous_runtime_t* runtime)
{
    if (!agent || !runtime || !runtime->graph) {
        return AEGIS_ERR_INVALID;
    }

    if (runtime->last_reflection) {
        aegis_reflection_destroy(runtime->last_reflection);
        runtime->last_reflection = NULL;
    }
    free(runtime->replan_feedback);
    runtime->replan_feedback = NULL;

    aegis_reflection_t* refl = NULL;
    aegis_status_t      rc   = aegis_reflection_create(&refl, runtime->graph);
    if (rc != AEGIS_OK) {
        return rc;
    }
    runtime->last_reflection = refl;

    const char* fb = aegis_reflection_feedback(refl);
    if (!fb || fb[0] == '\0') {
        fb = "plan failed, need revision";
    }
    runtime->replan_feedback = strdup(fb);
    if (!runtime->replan_feedback) {
        return AEGIS_ERR_NOMEM;
    }
    return AEGIS_OK;
}
