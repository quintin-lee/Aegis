#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <string.h>

aegis_status_t autonomous_evaluate(aegis_autonomous_agent_t*   agent,
                                   aegis_autonomous_runtime_t* runtime)
{
    if (!agent || !runtime) {
        return AEGIS_ERR_INVALID;
    }
    if (!agent->critic || !runtime->plan || !runtime->graph) {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token = autonomous_get_token(agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    memset(&runtime->last_critique, 0, sizeof(runtime->last_critique));
    aegis_status_t rc = aegis_critic_evaluate(agent->critic, runtime->goal, runtime->plan,
                                              runtime->graph, token, &runtime->last_critique);
    return rc;
}
