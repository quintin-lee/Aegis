#include "aegis/agent.h"
#include "aegis/status.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    aegis_agent_t* a = NULL;
    assert(aegis_agent_create(&a, "alpha") == AEGIS_OK);
    assert(a != NULL);
    aegis_agent_destroy(a);

    /* Invalid inputs */
    assert(aegis_agent_create(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_agent_create(&a, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_create(&a, "") == AEGIS_ERR_INVALID);

    /* NULL destroy is safe */
    aegis_agent_destroy(NULL);
    return 0;
}
