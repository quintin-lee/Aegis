/**
 * @file autonomous_state.c
 * @brief Autonomous agent state machine implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/autonomous_state.h"

const char* aegis_autonomous_state_str(aegis_autonomous_state_t state)
{
    switch (state) {
        case AEGIS_AUTO_CREATED:        return "CREATED";
        case AEGIS_AUTO_INITIALIZING:   return "INITIALIZING";
        case AEGIS_AUTO_READY:          return "READY";
        case AEGIS_AUTO_PLANNING:       return "PLANNING";
        case AEGIS_AUTO_SCHEDULING:     return "SCHEDULING";
        case AEGIS_AUTO_EXECUTING:      return "EXECUTING";
        case AEGIS_AUTO_EVALUATING:     return "EVALUATING";
        case AEGIS_AUTO_REFLECTING:     return "REFLECTING";
        case AEGIS_AUTO_REPLANNING:     return "REPLANNING";
        case AEGIS_AUTO_CHECKPOINTING:  return "CHECKPOINTING";
        case AEGIS_AUTO_RECOVERING:     return "RECOVERING";
        case AEGIS_AUTO_COMPLETED:      return "COMPLETED";
        case AEGIS_AUTO_FAILED:         return "FAILED";
        case AEGIS_AUTO_CANCELLING:     return "CANCELLING";
        case AEGIS_AUTO_CANCELLED:      return "CANCELLED";
        default:                        return "UNKNOWN";
    }
}
