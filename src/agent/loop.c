#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/agent/state.h"
#include "aegis/message/message.h"
#include "aegis/context/context.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>

struct aegis_agent_loop {
    aegis_session_t*            session;
    aegis_model_client_t*       model;
    aegis_tool_registry_t*      tools;
    char*                       system_prompt;
    aegis_cancellation_token_t* token;
    aegis_agent_loop_state_t    state;
    pthread_mutex_t             lock;
};

static void set_state(aegis_agent_loop_t* l, aegis_agent_loop_state_t ns)
{
    pthread_mutex_lock(&l->lock);
    l->state = ns;
    pthread_mutex_unlock(&l->lock);
}

aegis_status_t aegis_agent_loop_create(const aegis_agent_loop_config_t* cfg,
                                       aegis_agent_loop_t**             out)
{
    if (!cfg || !out || !cfg->session || !cfg->model) {
        return AEGIS_ERR_INVALID;
    }
    aegis_agent_loop_t* l = (aegis_agent_loop_t*)calloc(1, sizeof(*l));
    if (!l) {
        return AEGIS_ERR_NOMEM;
    }
    l->session = cfg->session;
    l->model   = cfg->model;
    l->tools   = cfg->tools;
    l->token   = cfg->token;
    l->state   = AEGIS_AGENT_LOOP_IDLE;
    if (cfg->system_prompt) {
        l->system_prompt = strdup(cfg->system_prompt);
    }
    pthread_mutex_init(&l->lock, NULL);
    *out = l;
    return AEGIS_OK;
}

void aegis_agent_loop_destroy(aegis_agent_loop_t* l)
{
    if (!l) {
        return;
    }
    free(l->system_prompt);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

aegis_agent_loop_state_t aegis_agent_loop_state(const aegis_agent_loop_t* l)
{
    if (!l) {
        return AEGIS_AGENT_LOOP_FAILED;
    }
    pthread_mutex_lock((pthread_mutex_t*)&l->lock);
    aegis_agent_loop_state_t s = l->state;
    pthread_mutex_unlock((pthread_mutex_t*)&l->lock);
    return s;
}

aegis_status_t aegis_agent_loop_cancel(aegis_agent_loop_t* l)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    if (l->token) {
        aegis_cancellation_token_request_cancel(l->token);
    }
    set_state(l, AEGIS_AGENT_LOOP_CANCELLING);
    return AEGIS_OK;
}
aegis_status_t aegis_agent_loop_pause(aegis_agent_loop_t* l)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    set_state(l, AEGIS_AGENT_LOOP_PAUSED);
    return AEGIS_OK;
}
aegis_status_t aegis_agent_loop_resume(aegis_agent_loop_t* l)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    set_state(l, AEGIS_AGENT_LOOP_RUNNING);
    return AEGIS_OK;
}

// Helper: build message list from session + system prompt via context engine
static aegis_status_t build_context_messages(aegis_agent_loop_t* l, aegis_message_list_t** out)
{
    // For now, directly use session messages plus system prompt
    aegis_message_list_t* list = NULL;
    aegis_status_t        st   = aegis_message_list_create(&list);
    if (st != AEGIS_OK) {
        return st;
    }
    if (l->system_prompt) {
        aegis_message_t* sys = NULL;
        aegis_message_create(AEGIS_MESSAGE_SYSTEM, &sys);
        aegis_message_set_content(sys, l->system_prompt);
        aegis_message_list_append(list, sys);
        aegis_message_destroy(sys);
    }
    size_t n = aegis_session_message_count(l->session);
    for (size_t i = 0; i < n; i++) {
        const aegis_message_t* m = aegis_session_message_at(l->session, i);
        aegis_message_list_append(list, m);
    }
    *out = list;
    return AEGIS_OK;
}

typedef struct stream_accum {
    char*               text;
    size_t              len;
    size_t              cap;
    aegis_tool_call_t** calls;
    size_t              call_count;
    size_t              call_cap;
} stream_accum_t;

static aegis_status_t stream_cb(const aegis_model_stream_event_t* ev, void* user)
{
    stream_accum_t* acc = (stream_accum_t*)user;
    if (!ev || !acc) {
        return AEGIS_ERR_INVALID;
    }
    if (ev->type == AEGIS_MODEL_STREAM_TEXT_DELTA && ev->data && ev->len) {
        if (acc->len + ev->len + 1 > acc->cap) {
            size_t ncap = acc->cap ? acc->cap * 2 : 256;
            while (ncap < acc->len + ev->len + 1) {
                ncap *= 2;
            }
            char* n = (char*)realloc(acc->text, ncap);
            if (!n) {
                return AEGIS_ERR_NOMEM;
            }
            acc->text = n;
            acc->cap  = ncap;
        }
        memcpy(acc->text + acc->len, ev->data, ev->len);
        acc->len += ev->len;
        acc->text[acc->len] = '\0';
    }
    // Tool call events are not yet produced by mock; placeholder
    return AEGIS_OK;
}

aegis_status_t aegis_agent_loop_run_turn(aegis_agent_loop_t* l, const char* user_input)
{
    if (!l || !user_input) {
        return AEGIS_ERR_INVALID;
    }
    if (l->token && aegis_cancellation_token_is_cancelled(l->token)) {
        return AEGIS_ERR_CANCELLED;
    }

    // Append user message
    aegis_message_t* um = NULL;
    aegis_status_t   st = aegis_message_create(AEGIS_MESSAGE_USER, &um);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_message_set_content(um, user_input);
    st = aegis_session_append_message(l->session, um);
    aegis_message_destroy(um);
    if (st != AEGIS_OK) {
        return st;
    }

    set_state(l, AEGIS_AGENT_LOOP_RUNNING);

    while (1) {
        if (l->token && aegis_cancellation_token_is_cancelled(l->token)) {
            set_state(l, AEGIS_AGENT_LOOP_CANCELLED);
            return AEGIS_ERR_CANCELLED;
        }

        // Build context
        set_state(l, AEGIS_AGENT_LOOP_WAITING_MODEL);
        aegis_message_list_t* ctx_msgs = NULL;
        st                             = build_context_messages(l, &ctx_msgs);
        if (st != AEGIS_OK) {
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return st;
        }

        aegis_model_request_t req = {
            .model       = "mock",
            .messages    = ctx_msgs,
            .tools       = l->tools,
            .max_tokens  = 0,
            .temperature = 0.7f,
            .stream      = true,
        };

        stream_accum_t acc = {0};
        st                 = aegis_model_stream(l->model, &req, l->token, stream_cb, &acc);
        aegis_message_list_destroy(ctx_msgs);
        if (st != AEGIS_OK) {
            free(acc.text);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return st;
        }

        // Append assistant message
        aegis_message_t* am = NULL;
        aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &am);
        aegis_message_set_content(am, acc.text ? acc.text : "");
        // Attach any tool calls collected (none in mock)
        for (size_t i = 0; i < acc.call_count; i++) {
            aegis_message_add_tool_call(am, acc.calls[i]);
            aegis_tool_call_destroy(acc.calls[i]);
        }
        free(acc.calls);
        free(acc.text);

        size_t tc = aegis_message_tool_call_count(am);
        st        = aegis_session_append_message(l->session, am);
        if (st != AEGIS_OK) {
            aegis_message_destroy(am);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return st;
        }

        if (tc == 0) {
            aegis_message_destroy(am);
            set_state(l, AEGIS_AGENT_LOOP_COMPLETED);
            return AEGIS_OK;
        }

        // Tool execution path
        set_state(l, AEGIS_AGENT_LOOP_WAITING_TOOL);
        for (size_t i = 0; i < tc; i++) {
            const aegis_tool_call_t* call = aegis_message_tool_call_at(am, i);
            const char*              name = aegis_tool_call_name(call);
            const char*              cid  = aegis_tool_call_id(call);
            // For now, mock tool result
            aegis_message_t* tr = NULL;
            aegis_message_create(AEGIS_MESSAGE_TOOL, &tr);
            aegis_message_set_tool_call_id(tr, cid);
            char result[256];
            snprintf(result, sizeof(result), "mock result for %s", name ? name : "unknown");
            aegis_message_set_content(tr, result);
            aegis_session_append_message(l->session, tr);
            aegis_message_destroy(tr);
            if (l->token && aegis_cancellation_token_is_cancelled(l->token)) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_CANCELLED);
                return AEGIS_ERR_CANCELLED;
            }
        }
        aegis_message_destroy(am);
        // loop continues for next model turn
    }
}

aegis_status_t aegis_agent_loop_run(aegis_agent_loop_t* l, const char* user_input)
{
    return aegis_agent_loop_run_turn(l, user_input);
}