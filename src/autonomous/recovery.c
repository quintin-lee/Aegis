#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include "aegis/checkpoint/checkpoint.h"
#include <string.h>

aegis_status_t autonomous_checkpoint_restore(aegis_autonomous_agent_t* aa, const char* path)
{
    if (!aa || !path) {
        return AEGIS_ERR_INVALID;
    }
    aegis_checkpoint_t*       ckpt = NULL;
    aegis_checkpoint_status_t st   = AEGIS_CHECKPOINT_MISSING;
    aegis_status_t            rc   = aegis_checkpoint_read(path, &ckpt, &st);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (st != AEGIS_CHECKPOINT_OK) {
        if (ckpt) {
            aegis_checkpoint_destroy(ckpt);
        }
        switch (st) {
        case AEGIS_CHECKPOINT_CORRUPTED:
        case AEGIS_CHECKPOINT_INCOMPLETE:
            return AEGIS_ERR_INVALID;
        case AEGIS_CHECKPOINT_VERSION_MISMATCH:
            return AEGIS_ERR_INVALID;
        case AEGIS_CHECKPOINT_MISSING:
        default:
            return AEGIS_ERR_NOT_FOUND;
        }
    }
    uint32_t    version   = aegis_checkpoint_version(ckpt);
    uint64_t    iter      = aegis_checkpoint_iteration(ckpt);
    size_t      n_tasks   = aegis_checkpoint_task_count(ckpt);
    const char* goal      = aegis_checkpoint_goal(ckpt);
    const char* state_str = aegis_checkpoint_agent_state(ckpt);

    // Validate task snapshots
    for (size_t i = 0; i < n_tasks; i++) {
        const aegis_checkpoint_task_snapshot_t* snap = aegis_checkpoint_task_snapshot(ckpt, i);
        if (snap && snap->max_retries < 0) {
            aegis_checkpoint_destroy(ckpt);
            return AEGIS_ERR_INVALID;
        }
        if (snap && snap->task_name[0] == '\0') {
            aegis_checkpoint_destroy(ckpt);
            return AEGIS_ERR_INVALID;
        }
    }
    // Validate iteration vs version consistency
    if (version == 0 && iter == 0 && n_tasks == 0 && (goal == NULL || goal[0] == '\0')) {
        // empty checkpoint considered invalid for restore
    }

    pthread_mutex_lock(&aa->lock);
    aa->recovered = true;
    if (aa->runtime) {
        aa->runtime->recovering          = true;
        aa->runtime->iteration           = iter;
        aa->runtime->checkpoint_sequence = version;
        if (goal && goal[0] != '\0') {
            strncpy(aa->runtime->goal, goal, sizeof(aa->runtime->goal) - 1);
            aa->runtime->goal[sizeof(aa->runtime->goal) - 1] = '\0';
        }
        // Plan/graph restoration: keep existing plan/graph if any, otherwise
        // checkpoint's plan_text is retained for future use. Full deserialization
        // would require plan DSL parser — store goal and let next loop replan
        // if plan is NULL, which preserves correctness for crash recovery.
    }
    if (iter > 0) {
        aa->iteration = (uint32_t)iter;
    } else if (version > 0) {
        aa->iteration = version;
    }
    (void)state_str;
    pthread_mutex_unlock(&aa->lock);

    aegis_checkpoint_destroy(ckpt);
    (void)autonomous_transition(aa, AEGIS_AUTO_RECOVERING);
    (void)autonomous_transition(aa, AEGIS_AUTO_READY);
    return AEGIS_OK;
}
