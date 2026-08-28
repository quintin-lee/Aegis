#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <stdlib.h>
#include <string.h>

aegis_status_t autonomous_reflect(aegis_autonomous_agent_t*   agent,
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
