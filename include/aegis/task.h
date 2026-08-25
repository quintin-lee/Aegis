#ifndef AEGIS_TASK_H
#define AEGIS_TASK_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aegis_task_state {
    AEGIS_TASK_PENDING,
    AEGIS_TASK_READY,
    AEGIS_TASK_RUNNING,
    AEGIS_TASK_DONE,
    AEGIS_TASK_FAILED,
    AEGIS_TASK_CANCELLED,
} aegis_task_state_t;

typedef struct aegis_task aegis_task_t;

aegis_status_t aegis_task_create(aegis_task_t **out, const char *desc);
void aegis_task_destroy(aegis_task_t *task);
aegis_task_state_t aegis_task_state(const aegis_task_t *task);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TASK_H */
