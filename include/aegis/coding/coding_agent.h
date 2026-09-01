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
                                                    aegis_session_t* session);
aegis_status_t   aegis_coding_agent_run(aegis_coding_agent_t* agent, const char* user_input);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_AGENT_H */
