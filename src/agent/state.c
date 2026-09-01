#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/state.h"

const char* aegis_agent_loop_state_str(aegis_agent_loop_state_t s)
{
    switch (s) {
    case AEGIS_AGENT_LOOP_IDLE:
        return "IDLE";
    case AEGIS_AGENT_LOOP_RUNNING:
        return "RUNNING";
    case AEGIS_AGENT_LOOP_WAITING_MODEL:
        return "WAITING_MODEL";
    case AEGIS_AGENT_LOOP_WAITING_TOOL:
        return "WAITING_TOOL";
    case AEGIS_AGENT_LOOP_COMPACTING:
        return "COMPACTING";
    case AEGIS_AGENT_LOOP_PAUSED:
        return "PAUSED";
    case AEGIS_AGENT_LOOP_CANCELLING:
        return "CANCELLING";
    case AEGIS_AGENT_LOOP_COMPLETED:
        return "COMPLETED";
    case AEGIS_AGENT_LOOP_FAILED:
        return "FAILED";
    case AEGIS_AGENT_LOOP_CANCELLED:
        return "CANCELLED";
    default:
        return "UNKNOWN";
    }
}

int aegis_agent_loop_state_is_terminal(aegis_agent_loop_state_t s)
{
    return s == AEGIS_AGENT_LOOP_COMPLETED || s == AEGIS_AGENT_LOOP_FAILED ||
           s == AEGIS_AGENT_LOOP_CANCELLED;
}
