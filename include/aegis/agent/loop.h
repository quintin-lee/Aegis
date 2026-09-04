#ifndef AEGIS_AGENT_LOOP_H
#define AEGIS_AGENT_LOOP_H

#include "aegis/agent/state.h"
#include "aegis/session/session.h"
#include "aegis/message/usage.h"
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

typedef enum aegis_tool_approval {
    AEGIS_TOOL_APPROVAL_ALLOW = 0,
    AEGIS_TOOL_APPROVAL_DENY  = 1,
} aegis_tool_approval_t;

/** Approval gate (control-flow callback, unlike the observer): invoked
 *  synchronously before each tool execution; the verdict decides whether
 *  the tool runs. DENY feeds "user denied tool <name>" back as the tool
 *  result so the turn continues. Invoked without loop locks held. */
typedef aegis_tool_approval_t (*aegis_tool_approval_fn)(const char* tool_name,
                                                        const char* arguments_json,
                                                        void*       user);

typedef struct aegis_agent_loop_config {
    aegis_session_t*            session;        // borrowed
    aegis_model_client_t*       model;          // borrowed
    aegis_tool_registry_t*      tools;          // borrowed
    const char*                 system_prompt;  // borrowed
    aegis_cancellation_token_t* token;          // borrowed
    aegis_agent_event_fn        on_event;       // optional observer, may be NULL
    void*                       event_user;     // borrowed, passed to on_event
    aegis_tool_approval_fn      tool_approval;  // optional gate, NULL = allow all
    void*                       approval_user;  // borrowed, passed to tool_approval
} aegis_agent_loop_config_t;

aegis_status_t aegis_agent_loop_create(const aegis_agent_loop_config_t* cfg,
                                       aegis_agent_loop_t**             out);
void           aegis_agent_loop_destroy(aegis_agent_loop_t* loop);

aegis_agent_loop_state_t aegis_agent_loop_state(const aegis_agent_loop_t* loop);

aegis_status_t aegis_agent_loop_run_turn(aegis_agent_loop_t* loop, const char* user_input);
aegis_status_t aegis_agent_loop_run(aegis_agent_loop_t* loop, const char* user_input);

aegis_status_t aegis_agent_loop_cancel(aegis_agent_loop_t* loop);

/** Rebind the loop's cancellation token at runtime (borrowed; NULL = none).
 *  Thread-safe; takes the loop lock briefly. */
aegis_status_t aegis_agent_loop_set_token(aegis_agent_loop_t*         loop,
                                          aegis_cancellation_token_t* token);
aegis_status_t aegis_agent_loop_pause(aegis_agent_loop_t* loop);
aegis_status_t aegis_agent_loop_resume(aegis_agent_loop_t* loop);

/**
 * @brief Rebind the event observer at runtime.
 * @param fn New callback (NULL disables event emission).
 * @return AEGIS_OK, or AEGIS_ERR_INVALID when loop is NULL.
 */
aegis_status_t aegis_agent_loop_set_event_callback(aegis_agent_loop_t* loop,
                                                   aegis_agent_event_fn fn, void* user);

/**
 * @brief Rebind the tool approval gate at runtime.
 * @param fn New gate (NULL restores implicit allow-all).
 * @return AEGIS_OK, or AEGIS_ERR_INVALID when loop is NULL.
 */
aegis_status_t aegis_agent_loop_set_tool_approval(aegis_agent_loop_t*    loop,
                                                  aegis_tool_approval_fn fn, void* user);

/** Copy the token usage of the most recent completed turn. Zeroes when the
 *  provider did not report usage. AEGIS_ERR_INVALID when loop or out is NULL. */
aegis_status_t aegis_agent_loop_last_usage(const aegis_agent_loop_t* loop, aegis_usage_t* out);

/** Copy lifetime token usage accumulated across all completed turns.
 *  AEGIS_ERR_INVALID when loop or out is NULL. */
aegis_status_t aegis_agent_loop_usage(const aegis_agent_loop_t* loop, aegis_usage_t* out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_LOOP_H */
