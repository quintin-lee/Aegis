#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <stdlib.h>
#include <string.h>

aegis_cancellation_token_t* autonomous_get_token(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return NULL;
    }
    if (aa->cfg.cancel_token) {
        return aa->cfg.cancel_token;
    }
    return aa->owned_token;
}

aegis_status_t autonomous_runtime_create(aegis_autonomous_runtime_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_autonomous_runtime_t* rt = calloc(1, sizeof(*rt));
    if (!rt) {
        return AEGIS_ERR_NOMEM;
    }
    rt->iteration           = 0;
    rt->checkpoint_sequence = 0;
    rt->tasks_executed      = 0;
    rt->tasks_failed        = 0;
    rt->tasks_retried       = 0;
    rt->plans_generated     = 0;
    rt->replans             = 0;
    rt->recovering          = false;
    *out                    = rt;
    return AEGIS_OK;
}

void autonomous_runtime_destroy(aegis_autonomous_runtime_t* rt)
{
    if (!rt) {
        return;
    }
    if (rt->plan) {
        aegis_plan_destroy(rt->plan);
    }
    if (rt->graph) {
        aegis_task_graph_destroy(rt->graph);
    }
    if (rt->last_reflection) {
        aegis_reflection_destroy(rt->last_reflection);
    }
    free(rt->replan_feedback);
    free(rt);
}

void autonomous_runtime_reset(aegis_autonomous_runtime_t* rt)
{
    if (!rt) {
        return;
    }
    if (rt->plan) {
        aegis_plan_destroy(rt->plan);
        rt->plan = NULL;
    }
    if (rt->graph) {
        aegis_task_graph_destroy(rt->graph);
        rt->graph = NULL;
    }
    if (rt->last_reflection) {
        aegis_reflection_destroy(rt->last_reflection);
        rt->last_reflection = NULL;
    }
    free(rt->replan_feedback);
    rt->replan_feedback = NULL;
    memset(&rt->last_critique, 0, sizeof(rt->last_critique));
    rt->recovering = false;
}

aegis_status_t autonomous_lifecycle_init(aegis_autonomous_agent_t* aa)
{
    (void)aa;
    return AEGIS_OK;
}

void autonomous_lifecycle_cleanup(aegis_autonomous_agent_t* aa)
{
    (void)aa;
}
