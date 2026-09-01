#ifndef AEGIS_AGENT_STATE_H
#define AEGIS_AGENT_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file state.h
 * @brief New Agent Loop state machine (reactive, not autonomous).
 */

typedef enum aegis_agent_loop_state {
    AEGIS_AGENT_LOOP_IDLE          = 0,
    AEGIS_AGENT_LOOP_RUNNING       = 1,
    AEGIS_AGENT_LOOP_WAITING_MODEL = 2,
    AEGIS_AGENT_LOOP_WAITING_TOOL  = 3,
    AEGIS_AGENT_LOOP_COMPACTING    = 4,
    AEGIS_AGENT_LOOP_PAUSED        = 5,
    AEGIS_AGENT_LOOP_CANCELLING    = 6,
    AEGIS_AGENT_LOOP_COMPLETED     = 7,
    AEGIS_AGENT_LOOP_FAILED        = 8,
    AEGIS_AGENT_LOOP_CANCELLED     = 9
} aegis_agent_loop_state_t;

const char* aegis_agent_loop_state_str(aegis_agent_loop_state_t s);
int         aegis_agent_loop_state_is_terminal(aegis_agent_loop_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_STATE_H */
