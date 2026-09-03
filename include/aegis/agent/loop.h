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

/**
 * @brief Typed observer events emitted by the agent loop.
 *
 * Events are informational only: the callback cannot alter loop control
 * flow, and event payloads are borrowed and valid only for the duration
 * of the callback.
 */
typedef enum aegis_agent_event_type {
    AEGIS_AGENT_EVENT_TEXT_DELTA = 0, /**< data/len: borrowed text fragment   */
    AEGIS_AGENT_EVENT_TOOL_START = 1, /**< tool_name/call_id set               */
    AEGIS_AGENT_EVENT_TOOL_END   = 2, /**< tool_name/call_id set; status = tool
                                           outcome; data/len = borrowed result
                                           string when status == AEGIS_OK      */
    AEGIS_AGENT_EVENT_REASONING_DELTA = 3, /**< data/len: borrowed reasoning fragment */
} aegis_agent_event_type_t;

typedef struct aegis_agent_event {
    aegis_agent_event_type_t type;
    const void*              data;      /**< Borrowed; valid during callback only */
    size_t                   len;
    const char*              tool_name; /**< TOOL_* events only                   */
    const char*              call_id;   /**< TOOL_* events only                   */
    aegis_status_t           status;    /**< TOOL_END: tool outcome               */
} aegis_agent_event_t;

/** Observer callback. Invoked without loop locks held; must not retain
 *  borrowed event data past the call. */
typedef void (*aegis_agent_event_fn)(const aegis_agent_event_t* ev, void* user);

typedef struct aegis_agent_loop_config {
    aegis_session_t*            session;        // borrowed
    aegis_model_client_t*       model;          // borrowed
    aegis_tool_registry_t*      tools;          // borrowed
    const char*                 system_prompt;  // borrowed
    aegis_cancellation_token_t* token;          // borrowed
    aegis_agent_event_fn        on_event;       // optional observer, may be NULL
    void*                       event_user;     // borrowed, passed to on_event
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

/**
 * @brief Rebind the event observer at runtime.
 * @param fn New callback (NULL disables event emission).
 * @return AEGIS_OK, or AEGIS_ERR_INVALID when loop is NULL.
 */
aegis_status_t aegis_agent_loop_set_event_callback(aegis_agent_loop_t* loop,
                                                   aegis_agent_event_fn fn, void* user);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_LOOP_H */
