#ifndef AEGIS_CODING_AGENT_H
#define AEGIS_CODING_AGENT_H

#include "aegis/session/session.h"
#include "aegis/model/model.h"
#include "aegis/tool/tool.h"
#include "aegis/agent/loop.h"
#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file coding_agent.h
 * @brief Pi-like Coding Agent — project-aware, tool-rich, session-backed.
 */

typedef struct aegis_coding_agent aegis_coding_agent_t;

typedef struct aegis_coding_agent_config {
    const char*            project_root;
    const char*            model;
    const char*            provider;
    const char*            api_key;
    const char*            base_url;
    aegis_tool_registry_t* tools;  // borrowed, if NULL creates default coding tools
} aegis_coding_agent_config_t;

aegis_status_t aegis_coding_agent_create(const aegis_coding_agent_config_t* cfg,
                                         aegis_coding_agent_t**             out);
void           aegis_coding_agent_destroy(aegis_coding_agent_t* agent);

aegis_session_t* aegis_coding_agent_session(aegis_coding_agent_t* agent);
aegis_status_t   aegis_coding_agent_replace_session(aegis_coding_agent_t* agent,
                                                    aegis_session_t*      session);
aegis_status_t   aegis_coding_agent_run(aegis_coding_agent_t* agent, const char* user_input);

/**
 * @brief Current model name (owned by the agent; valid until set_model).
 * @return Borrowed string, or NULL when agent is NULL.
 */
const char* aegis_coding_agent_model_name(const aegis_coding_agent_t* agent);

/**
 * @brief Switch the model used for subsequent turns.
 *
 * Rebuilds the model client from the agent's stored provider configuration
 * (OpenAI backend when configured, mock otherwise) and swaps in a fresh
 * agent loop. The session is untouched. On failure the previous model and
 * loop remain fully functional.
 *
 * @param model New model name (non-NULL, non-empty).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, or the client/loop creation error.
 */
aegis_status_t aegis_coding_agent_set_model(aegis_coding_agent_t* agent, const char* model);

/**
 * @brief Register (or clear) the agent-loop event observer.
 *
 * The callback receives streaming text deltas and tool start/end events
 * as they happen; see aegis_agent_event_fn for the payload contract.
 * Pass fn == NULL to disable event emission.
 *
 * @return AEGIS_OK, or AEGIS_ERR_INVALID when agent is NULL.
 */
aegis_status_t aegis_coding_agent_set_event_callback(aegis_coding_agent_t* agent,
                                                     aegis_agent_event_fn  fn,
                                                     void*                 user);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_AGENT_H */
