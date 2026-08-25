/**
 * @file task.c
 * @brief Task creation, destruction and state query.
 *
 * A task owns its description string (strdup'd on create, freed on destroy).
 * State transitions are managed externally by the scheduler/executor;
 * this module only exposes the current state via aegis_task_state().
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/task.h"
#include <stdlib.h>
#include <string.h>

struct aegis_task {
    char*              description; /* owned */
    aegis_task_state_t state;
};

aegis_status_t aegis_task_create(aegis_task_t** out, const char* desc)
{
    if (!out || !desc) {
        return AEGIS_ERR_INVALID;
    }
    aegis_task_t* t = calloc(1, sizeof(*t));
    if (!t) {
        return AEGIS_ERR_NOMEM;
    }
    t->description = strdup(desc);
    if (!t->description) {
        free(t);
        return AEGIS_ERR_NOMEM;
    }
    t->state = AEGIS_TASK_PENDING;
    *out     = t;
    return AEGIS_OK;
}

void aegis_task_destroy(aegis_task_t* task)
{
    if (!task) {
        return;
    }
    free(task->description);
    free(task);
}

aegis_task_state_t aegis_task_state(const aegis_task_t* task)
{
    return task ? task->state : AEGIS_TASK_PENDING;
}
