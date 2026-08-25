#include "aegis/scheduler.h"
#include <assert.h>

int main(void)
{
    aegis_scheduler_t* s = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(s != NULL);
    aegis_scheduler_destroy(s);
    aegis_scheduler_destroy(NULL);
    assert(aegis_scheduler_create(NULL) == AEGIS_ERR_INVALID);
    return 0;
}
