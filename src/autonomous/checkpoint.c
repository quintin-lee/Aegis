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
    /* Snapshot boundary: hold lock while copying iteration/state for consistency. */
    uint32_t                   iteration = 0;
    aegis_autonomous_state_t st        = AEGIS_AUTO_CREATED;
    pthread_mutex_lock(&aa->lock);
    iteration = aa->iteration;
    st        = aa->state;
    pthread_mutex_unlock(&aa->lock);
    const char* state_str = aegis_autonomous_state_str(st);
    aegis_checkpoint_populate(ckpt, state_str, goal, plan, graph, iteration + 1);
    aegis_checkpoint_write(ckpt, path, token);
    aegis_checkpoint_destroy(ckpt);
}
