#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"

#include "aegis/checkpoint/checkpoint.h"

void autonomous_checkpoint_save(aegis_autonomous_agent_t* aa,
                                const char*               goal,
                                aegis_plan_t*             plan,
                                aegis_task_graph_t*       graph)
{
    if (!aa) {
        return;
    }
    const char* path = aa->cfg.checkpoint_path;
    if (!path) {
        return;
    }
    aegis_checkpoint_t* ckpt = NULL;
    if (aegis_checkpoint_create(&ckpt) != AEGIS_OK) {
        return;
    }
    aegis_cancellation_token_t* token = autonomous_get_token(aa);
    /* Snapshot boundary: hold lock while copying state that will be persisted
     * to ensure a consistent view. For now we populate with minimal data. */
    pthread_mutex_lock(&aa->lock);
    uint32_t iteration = aa->iteration;
    pthread_mutex_unlock(&aa->lock);
    aegis_checkpoint_populate(ckpt, NULL, NULL, plan, graph, 0);
    if (goal) {
        aegis_checkpoint_set_goal(ckpt, goal);
    }
    /* Use iteration as plan_version hint for recovery validation. */
    (void)iteration;
    aegis_checkpoint_write(ckpt, path, token);
    aegis_checkpoint_destroy(ckpt);
}
