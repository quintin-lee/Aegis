#include "aegis/task.h"
#include <assert.h>

int main(void)
{
    aegis_task_t* t = NULL;
    assert(aegis_task_create(&t, "test task") == AEGIS_OK);
    assert(t != NULL);
    assert(aegis_task_state(t) == AEGIS_TASK_PENDING);
    aegis_task_destroy(t);

    /* NULL input must return error */
    assert(aegis_task_create(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_task_create(&t, NULL) == AEGIS_ERR_INVALID);

    /* NULL destroy is safe */
    aegis_task_destroy(NULL);
    return 0;
}
