#include "aegis/scheduler.h"
#include <stdlib.h>

struct aegis_scheduler {
    int _reserved; /* opaque */
};

aegis_status_t aegis_scheduler_create(aegis_scheduler_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_scheduler_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    *out = s;
    return AEGIS_OK;
}

void aegis_scheduler_destroy(aegis_scheduler_t* sched)
{
    free(sched);
}
