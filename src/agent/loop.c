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
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

struct aegis_agent_loop {
    aegis_session_t*            session;
    aegis_model_client_t*       model;
    aegis_tool_registry_t*      tools;
    char*                       system_prompt;
    aegis_cancellation_token_t* token;
    aegis_agent_loop_state_t    state;
    aegis_agent_event_fn        on_event;
    void*                       event_user;
    aegis_tool_approval_fn      tool_approval;
    void*                       approval_user;
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
    l->session    = cfg->session;
    l->model      = cfg->model;
    l->tools      = cfg->tools;
    l->token      = cfg->token;
    l->on_event       = cfg->on_event;
    l->event_user     = cfg->event_user;
    l->tool_approval  = cfg->tool_approval;
    l->approval_user  = cfg->approval_user;
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

aegis_status_t aegis_agent_loop_set_token(aegis_agent_loop_t* l, aegis_cancellation_token_t* token)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&l->lock);
    l->token = token;
    pthread_mutex_unlock(&l->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_agent_loop_set_event_callback(aegis_agent_loop_t* l, aegis_agent_event_fn fn,
                                                   void* user)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&l->lock);
    l->on_event   = fn;
    l->event_user = user;
    pthread_mutex_unlock(&l->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_agent_loop_set_tool_approval(aegis_agent_loop_t*    l,
                                                  aegis_tool_approval_fn fn, void* user)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&l->lock);
    l->tool_approval = fn;
    l->approval_user = user;
    pthread_mutex_unlock(&l->lock);
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

// Build a bounded request history while preserving message roles, tool calls,
// and protocol ordering. Structured messages cannot be represented losslessly
// by the generic context-section builder.
static aegis_status_t build_context_messages(aegis_agent_loop_t* l, aegis_message_list_t** out)
{
    if (!l || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out                       = NULL;
    aegis_message_list_t* list = NULL;
    aegis_status_t        st   = aegis_message_list_create(&list);
    if (st != AEGIS_OK) {
        return st;
    }
    if (l->system_prompt) {
        aegis_message_t* sys = NULL;
        st                   = aegis_message_create(AEGIS_MESSAGE_SYSTEM, &sys);
        if (st == AEGIS_OK) {
            st = aegis_message_set_content(sys, l->system_prompt);
        }
        if (st == AEGIS_OK) {
            st = aegis_message_list_append(list, sys);
        }
        aegis_message_destroy(sys);
        if (st != AEGIS_OK) {
            aegis_message_list_destroy(list);
            return st;
        }
    }
    size_t n     = aegis_session_message_count(l->session);
    size_t first = n > 128 ? n - 128 : 0;
    for (size_t i = first; i < n; ++i) {
        if (l->token && aegis_cancellation_token_is_cancelled(l->token)) {
            aegis_message_list_destroy(list);
            return AEGIS_ERR_CANCELLED;
        }
        const aegis_message_t* msg = aegis_session_message_at(l->session, i);
        if (!msg) {
            continue;
        }
        st = aegis_message_list_append(list, msg);
        if (st != AEGIS_OK) {
            aegis_message_list_destroy(list);
            return st;
        }
    }
    *out = list;
    return AEGIS_OK;
}

typedef struct stream_call_accum {
    uint32_t index;
    char*    call_id;
    char*    name;
    char*    arguments;
    size_t   arguments_len;
    size_t   arguments_cap;
} stream_call_accum_t;

typedef struct stream_accum {
    char*                text;
    size_t               len;
    size_t               cap;
    char*                reasoning;
    size_t               rlen;
    size_t               rcap;
    stream_call_accum_t* calls;
    size_t               call_count;
    size_t               call_cap;
    aegis_agent_loop_t*  loop; /**< Borrowed; set per run for event forwarding. */
} stream_accum_t;

static void stream_accum_destroy(stream_accum_t* acc)
{
    if (!acc) {
        return;
    }
    free(acc->text);
    free(acc->reasoning);
    for (size_t i = 0; i < acc->call_count; ++i) {
        free(acc->calls[i].call_id);
        free(acc->calls[i].name);
        free(acc->calls[i].arguments);
    }
    free(acc->calls);
    memset(acc, 0, sizeof(*acc));
}

static stream_call_accum_t* stream_call_get(stream_accum_t* acc, uint32_t index)
{
    for (size_t i = 0; i < acc->call_count; ++i) {
        if (acc->calls[i].index == index) {
            return &acc->calls[i];
        }
    }
    if (acc->call_count == acc->call_cap) {
        size_t               cap = acc->call_cap ? acc->call_cap * 2 : 4;
        stream_call_accum_t* p   = realloc(acc->calls, cap * sizeof(*p));
        if (!p) {
            return NULL;
        }
        acc->calls    = p;
        acc->call_cap = cap;
    }
    stream_call_accum_t* c = &acc->calls[acc->call_count++];
    memset(c, 0, sizeof(*c));
    c->index = index;
    return c;
}

static int replace_string(char** dst, const char* src)
{
    if (!src) {
        return 1;
    }
    char* copy = strdup(src);
    if (!copy) {
        return 0;
    }
    free(*dst);
    *dst = copy;
    return 1;
}

static int append_call_args(stream_call_accum_t* c, const void* data, size_t len)
{
    if (!len) {
        return 1;
    }
    if (len > SIZE_MAX - c->arguments_len - 1) {
        return 0;
    }
    size_t need = c->arguments_len + len + 1;
    if (need > c->arguments_cap) {
        size_t cap = c->arguments_cap ? c->arguments_cap * 2 : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                return 0;
            }
            cap *= 2;
        }
        char* p = realloc(c->arguments, cap);
        if (!p) {
            return 0;
        }
        c->arguments     = p;
        c->arguments_cap = cap;
    }
    memcpy(c->arguments + c->arguments_len, data, len);
    c->arguments_len += len;
    c->arguments[c->arguments_len] = '\0';
    return 1;
}

static int json_skip_ws(const char** p, const char* end)
{
    while (*p < end && isspace((unsigned char)**p)) {
        ++*p;
    }
    return *p < end;
}

static int json_parse_string(const char** p, const char* end, char** out)
{
    if (!json_skip_ws(p, end) || **p != '"') {
        return 0;
    }
    ++*p;
    size_t cap = 32, len = 0;
    char*  s = malloc(cap);
    if (!s) {
        return 0;
    }
    while (*p < end && **p != '"') {
        unsigned char ch = (unsigned char)*(*p)++;
        if (ch == '\\') {
            if (*p >= end) {
                free(s);
                return 0;
            }
            ch = (unsigned char)*(*p)++;
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch != '"' && ch != '\\' && ch != '/') {
                free(s);
                return 0;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char* n = realloc(s, cap);
            if (!n) {
                free(s);
                return 0;
            }
            s = n;
        }
        s[len++] = (char)ch;
    }
    if (*p >= end || **p != '"') {
        free(s);
        return 0;
    }
    ++*p;
    s[len] = '\0';
    *out   = s;
    return 1;
}

static int json_parse_args(const char* json, aegis_tool_args_t** out)
{
    if (!json || !out) {
        return 0;
    }
    *out            = NULL;
    const char* p   = json;
    const char* end = json + strlen(json);
    while (p < end && isspace((unsigned char)*p)) {
        ++p;
    }
    if (p >= end || *p++ != '{') {
        return 0;
    }
    aegis_tool_args_t* args = NULL;
    if (aegis_tool_args_create(&args) != AEGIS_OK) {
        return 0;
    }
    while (1) {
        while (p < end && isspace((unsigned char)*p)) {
            ++p;
        }
        if (p >= end) {
            aegis_tool_args_destroy(args);
            return 0;
        }
        if (*p == '}') {
            ++p;
            break;
        }
        char* key = NULL;
        if (!json_parse_string(&p, end, &key)) {
            free(key);
            aegis_tool_args_destroy(args);
            return 0;
        }
        while (p < end && isspace((unsigned char)*p)) {
            ++p;
        }
        if (p >= end || *p++ != ':') {
            free(key);
            aegis_tool_args_destroy(args);
            return 0;
        }
        while (p < end && isspace((unsigned char)*p)) {
            ++p;
        }
        if (p >= end) {
            free(key);
            aegis_tool_args_destroy(args);
            return 0;
        }
        aegis_status_t st = AEGIS_ERR_INVALID;
        if (*p == '"') {
            char* value = NULL;
            if (json_parse_string(&p, end, &value)) {
                st = aegis_tool_args_add_string(args, key, value);
            }
            free(value);
        } else if (*p == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0) {
            p += 4;
            st = aegis_tool_args_add_bool(args, key, true);
        } else if (*p == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) {
            p += 5;
            st = aegis_tool_args_add_bool(args, key, false);
        } else {
            errno             = 0;
            char*  number_end = NULL;
            double number     = strtod(p, &number_end);
            if (number_end == p || errno == ERANGE || number != number) {
                st = AEGIS_ERR_INVALID;
            } else {
                bool is_float = false;
                for (const char* q = p; q < number_end; ++q) {
                    if (*q == '.' || *q == 'e' || *q == 'E') {
                        is_float = true;
                        break;
                    }
                }
                if (is_float) {
                    st = aegis_tool_args_add_float(args, key, number);
                } else {
                    st = aegis_tool_args_add_int(args, key, (int64_t)number);
                }
            }
            p = number_end;
        }
        free(key);
        while (p < end && isspace((unsigned char)*p)) {
            ++p;
        }
        if (st != AEGIS_OK || p >= end) {
            aegis_tool_args_destroy(args);
            return 0;
        }
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') {
            ++p;
            break;
        }
        aegis_tool_args_destroy(args);
        return 0;
    }
    while (p < end && isspace((unsigned char)*p)) {
        ++p;
    }
    if (p != end) {
        aegis_tool_args_destroy(args);
        return 0;
    }
    *out = args;
    return 1;
}

static int accum_append(char** buf, size_t* len, size_t* cap, const char* data, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 256;
        while (ncap < *len + n + 1) {
            ncap *= 2;
        }
        char* nb = (char*)realloc(*buf, ncap);
        if (!nb) {
            return 0;
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static aegis_status_t stream_cb(const aegis_model_stream_event_t* ev, void* user)
{
    stream_accum_t* acc = (stream_accum_t*)user;
    if (!ev || !acc) {
        return AEGIS_ERR_INVALID;
    }
    if (ev->type == AEGIS_MODEL_STREAM_REASONING_DELTA && ev->data && ev->len) {
        if (!accum_append(&acc->reasoning, &acc->rlen, &acc->rcap, ev->data, ev->len)) {
            return AEGIS_ERR_NOMEM;
        }
        if (acc->loop && acc->loop->on_event) {
            aegis_agent_event_t out_ev = {
                .type = AEGIS_AGENT_EVENT_REASONING_DELTA,
                .data = ev->data,
                .len  = ev->len,
            };
            /* Observer runs without loop locks; it cannot affect control flow. */
            acc->loop->on_event(&out_ev, acc->loop->event_user);
        }
    }
    if (ev->type == AEGIS_MODEL_STREAM_TEXT_DELTA && ev->data && ev->len) {
        if (!accum_append(&acc->text, &acc->len, &acc->cap, ev->data, ev->len)) {
            return AEGIS_ERR_NOMEM;
        }
        if (acc->loop && acc->loop->on_event) {
            aegis_agent_event_t out_ev = {
                .type = AEGIS_AGENT_EVENT_TEXT_DELTA,
                .data = ev->data,
                .len  = ev->len,
            };
            /* Observer runs without loop locks; it cannot affect control flow. */
            acc->loop->on_event(&out_ev, acc->loop->event_user);
        }
    }
    if (ev->type == AEGIS_MODEL_STREAM_TOOL_CALL_START ||
        ev->type == AEGIS_MODEL_STREAM_TOOL_CALL_DELTA ||
        ev->type == AEGIS_MODEL_STREAM_TOOL_CALL_END) {
        stream_call_accum_t* call = stream_call_get(acc, ev->index);
        if (!call) {
            return AEGIS_ERR_NOMEM;
        }
        /* Empty strings mean "not provided in this chunk"; keep prior values. */
        if (ev->call_id && *ev->call_id && !replace_string(&call->call_id, ev->call_id)) {
            return AEGIS_ERR_NOMEM;
        }
        if (ev->tool_name && *ev->tool_name && !replace_string(&call->name, ev->tool_name)) {
            return AEGIS_ERR_NOMEM;
        }
        if (ev->type == AEGIS_MODEL_STREAM_TOOL_CALL_DELTA &&
            !append_call_args(call, ev->data, ev->len)) {
            return AEGIS_ERR_NOMEM;
        }
    }
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

    for (unsigned turn = 0; turn < 16; ++turn) {
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
            .model       = NULL,
            .messages    = ctx_msgs,
            .tools       = l->tools,
            .max_tokens  = 0,
            .temperature = 0.7f,
            .stream      = true,
        };

        stream_accum_t acc = {0};
        acc.loop           = l;
        st                 = aegis_model_stream(l->model, &req, l->token, stream_cb, &acc);
        aegis_message_list_destroy(ctx_msgs);
        if (st != AEGIS_OK) {
            if (st == AEGIS_ERR_CANCELLED) {
                /* Preserve whatever streamed before the interrupt, then
                 * mark it so the next turn has the interruption context. */
                aegis_message_t* pm = NULL;
                if (aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &pm) == AEGIS_OK &&
                    aegis_message_set_content(pm, acc.text ? acc.text : "") == AEGIS_OK) {
                    if (acc.reasoning) {
                        aegis_message_set_reasoning(pm, acc.reasoning);
                    }
                    for (size_t i = 0; i < acc.call_count; i++) {
                        aegis_tool_call_t* call = NULL;
                        if (aegis_tool_call_create(&call) == AEGIS_OK &&
                            aegis_tool_call_set_id(call, acc.calls[i].call_id) == AEGIS_OK &&
                            aegis_tool_call_set_name(call, acc.calls[i].name) == AEGIS_OK &&
                            aegis_tool_call_set_arguments(
                                call, acc.calls[i].arguments ? acc.calls[i].arguments : "{}") ==
                                AEGIS_OK &&
                            aegis_message_add_tool_call(pm, call) == AEGIS_OK) {
                            /* attached */
                        }
                        aegis_tool_call_destroy(call);
                    }
                    aegis_session_append_message(l->session, pm);
                    aegis_message_t* mk = NULL;
                    if (aegis_message_create(AEGIS_MESSAGE_USER, &mk) == AEGIS_OK &&
                        aegis_message_set_content(mk, "[interrupted by user]") == AEGIS_OK) {
                        aegis_session_append_message(l->session, mk);
                        aegis_message_destroy(mk);
                    }
                    aegis_message_destroy(pm);
                }
                stream_accum_destroy(&acc);
                set_state(l, AEGIS_AGENT_LOOP_CANCELLED);
                return AEGIS_ERR_CANCELLED;
            }
            stream_accum_destroy(&acc);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return st;
        }

        // Append assistant message
        aegis_message_t* am = NULL;
        if (aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &am) != AEGIS_OK) {
            stream_accum_destroy(&acc);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return AEGIS_ERR_NOMEM;
        }
        if (aegis_message_set_content(am, acc.text ? acc.text : "") != AEGIS_OK) {
            aegis_message_destroy(am);
            stream_accum_destroy(&acc);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return AEGIS_ERR_NOMEM;
        }
        if (acc.reasoning && aegis_message_set_reasoning(am, acc.reasoning) != AEGIS_OK) {
            aegis_message_destroy(am);
            stream_accum_destroy(&acc);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return AEGIS_ERR_NOMEM;
        }
        // Attach any tool calls collected (none in mock)
        for (size_t i = 0; i < acc.call_count; i++) {
            aegis_tool_call_t* call = NULL;
            if (aegis_tool_call_create(&call) != AEGIS_OK ||
                aegis_tool_call_set_id(call, acc.calls[i].call_id) != AEGIS_OK ||
                aegis_tool_call_set_name(call, acc.calls[i].name) != AEGIS_OK ||
                aegis_tool_call_set_arguments(
                    call, acc.calls[i].arguments ? acc.calls[i].arguments : "{}") != AEGIS_OK ||
                aegis_message_add_tool_call(am, call) != AEGIS_OK) {
                aegis_tool_call_destroy(call);
                aegis_message_destroy(am);
                stream_accum_destroy(&acc);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return AEGIS_ERR_NOMEM;
            }
            aegis_tool_call_destroy(call);
        }
        stream_accum_destroy(&acc);

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
            if (!l->tools || !name || !cid) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return AEGIS_ERR_TOOL;
            }
            aegis_tool_def_t def;
            st = aegis_tool_registry_find(l->tools, name, &def);
            if (st != AEGIS_OK) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return st;
            }
            aegis_tool_args_t* args     = NULL;
            const char*        raw_args = aegis_tool_call_arguments(call);
            if (!json_parse_args(raw_args ? raw_args : "{}", &args)) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return AEGIS_ERR_INVALID;
            }
            if (l->on_event) {
                aegis_agent_event_t ev = {
                    .type = AEGIS_AGENT_EVENT_TOOL_START,
                    .tool_name = name,
                    .call_id = cid,
                };
                l->on_event(&ev, l->event_user);
            }
            aegis_tool_result_t result = {0};
            aegis_tool_approval_t verdict = AEGIS_TOOL_APPROVAL_ALLOW;
            if (l->tool_approval) {
                verdict = l->tool_approval(name, raw_args ? raw_args : "{}", l->approval_user);
            }
            if (verdict == AEGIS_TOOL_APPROVAL_DENY) {
                char denial[128];
                snprintf(denial, sizeof(denial), "user denied tool %s", name);
                st = aegis_tool_result_set_string(&result, denial);
            } else {
                st = aegis_tool_execute(l->tools, name, args, l->token, &result);
            }
            aegis_tool_args_destroy(args);
            aegis_message_t* tr = NULL;
            if (aegis_message_create(AEGIS_MESSAGE_TOOL, &tr) != AEGIS_OK ||
                aegis_message_set_tool_call_id(tr, cid) != AEGIS_OK) {
                aegis_message_destroy(tr);
                aegis_tool_result_destroy(&result);
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return AEGIS_ERR_NOMEM;
            }
            if (verdict == AEGIS_TOOL_APPROVAL_DENY) {
                /* Denial is a normal tool outcome (not an error status). */
                aegis_message_set_content(tr,
                                          result.value.as.str.ptr ? result.value.as.str.ptr : "");
            } else if (st == AEGIS_OK && result.value.type == AEGIS_TOOL_VAL_STRING) {
                aegis_message_set_content(tr,
                                          result.value.as.str.ptr ? result.value.as.str.ptr : "");
            } else {
                char error[128];
                snprintf(error, sizeof(error), "tool %s failed: %s", name, aegis_status_str(st));
                aegis_message_set_content(tr, error);
            }
            if (l->on_event) {
                aegis_agent_event_t ev = {
                    .type      = AEGIS_AGENT_EVENT_TOOL_END,
                    .tool_name = name,
                    .call_id   = cid,
                    .status    = (verdict == AEGIS_TOOL_APPROVAL_DENY) ? AEGIS_ERR_PERM : st,
                };
                if (st == AEGIS_OK && result.value.type == AEGIS_TOOL_VAL_STRING &&
                    result.value.as.str.ptr) {
                    ev.data = result.value.as.str.ptr;
                    ev.len  = result.value.as.str.len;
                }
                l->on_event(&ev, l->event_user);
            }
            aegis_tool_result_destroy(&result);
            st = aegis_session_append_message(l->session, tr);
            aegis_message_destroy(tr);
            if (st != AEGIS_OK) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_FAILED);
                return st;
            }
            if (l->token && aegis_cancellation_token_is_cancelled(l->token)) {
                aegis_message_destroy(am);
                set_state(l, AEGIS_AGENT_LOOP_CANCELLED);
                return AEGIS_ERR_CANCELLED;
            }
        }
        aegis_message_destroy(am);
        // loop continues for next model turn
    }
    set_state(l, AEGIS_AGENT_LOOP_FAILED);
    return AEGIS_ERR_TIMEOUT;
}

aegis_status_t aegis_agent_loop_run(aegis_agent_loop_t* l, const char* user_input)
{
    return aegis_agent_loop_run_turn(l, user_input);
}