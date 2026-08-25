#define _POSIX_C_SOURCE 200809L
#include "aegis/agent.h"
#include "aegis/status.h"
#include "internal/lifecycle.h"
#include <stdlib.h>
#include <string.h>

struct aegis_agent {
    char* name; /* owned */
    /* future: scheduler, planner, memory, providers, etc. */
};

aegis_status_t aegis_agent_create(aegis_agent_t** out, const char* name)
{
    if (!out || !name || name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    aegis_agent_t* agent = calloc(1, sizeof(*agent));
    if (!agent) {
        return AEGIS_ERR_NOMEM;
    }
    agent->name = strdup(name);
    if (!agent->name) {
        free(agent);
        return AEGIS_ERR_NOMEM;
    }
    *out = agent;
    return AEGIS_OK;
}

void aegis_agent_destroy(aegis_agent_t* agent)
{
    if (!agent) {
        return;
    }
    free(agent->name);
    free(agent);
}
