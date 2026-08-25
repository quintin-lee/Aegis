#ifndef AEGIS_SCHEDULER_H
#define AEGIS_SCHEDULER_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aegis_scheduler aegis_scheduler_t;

aegis_status_t aegis_scheduler_create(aegis_scheduler_t** out);
void           aegis_scheduler_destroy(aegis_scheduler_t* sched);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SCHEDULER_H */
