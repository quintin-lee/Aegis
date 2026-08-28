#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"

aegis_cancellation_token_t* autonomous_get_token(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return NULL;
    }
    if (aa->cfg.cancel_token) {
        return aa->cfg.cancel_token;
    }
    return aa->owned_token;
}

aegis_status_t autonomous_lifecycle_init(aegis_autonomous_agent_t* aa)
{
    (void)aa;
    return AEGIS_OK;
}

void autonomous_lifecycle_cleanup(aegis_autonomous_agent_t* aa)
{
    (void)aa;
}
