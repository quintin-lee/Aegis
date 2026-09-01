#ifndef AEGIS_AGENT_LOOP_H
#define AEGIS_AGENT_LOOP_H

#include "aegis/agent/state.h"
#include "aegis/session/session.h"
#include "aegis/model/model.h"
#include "aegis/tool/tool.h"
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file loop.h
 * @brief Reactive Agent Loop — core of new architecture.
 */

typedef struct aegis_agent_loop aegis_agent_loop_t;

typedef struct aegis_agent_loop_config {
    aegis_session_t*            session;        // borrowed
    aegis_model_client_t*       model;          // borrowed
    aegis_tool_registry_t*      tools;          // borrowed
    const char*                 system_prompt;  // borrowed
    aegis_cancellation_token_t* token;          // borrowed
} aegis_agent_loop_config_t;

aegis_status_t aegis_agent_loop_create(const aegis_agent_loop_config_t* cfg,
                                       aegis_agent_loop_t**             out);
void           aegis_agent_loop_destroy(aegis_agent_loop_t* loop);

aegis_agent_loop_state_t aegis_agent_loop_state(const aegis_agent_loop_t* loop);

aegis_status_t aegis_agent_loop_run_turn(aegis_agent_loop_t* loop, const char* user_input);
aegis_status_t aegis_agent_loop_run(aegis_agent_loop_t* loop, const char* user_input);

aegis_status_t aegis_agent_loop_cancel(aegis_agent_loop_t* loop);
aegis_status_t aegis_agent_loop_pause(aegis_agent_loop_t* loop);
aegis_status_t aegis_agent_loop_resume(aegis_agent_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_LOOP_H */
