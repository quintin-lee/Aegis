#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
#include "aegis/tool/tool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Event log ────────────────────────────────────────────────────────── */

typedef struct ev_rec {
    aegis_agent_event_type_t type;
    char           tool_name[64];
    aegis_status_t status;
    char           text[64];
} ev_rec_t;

typedef struct ev_log {
    ev_rec_t items[32];
    size_t   n;
} ev_log_t;

static void ev_push(ev_log_t* log, aegis_agent_event_type_t t, const char* tool,
                    aegis_status_t st, const char* text)
{
    assert(log->n < 32);
    ev_rec_t* r = &log->items[log->n++];
    r->type   = t;
    r->status = st;
    snprintf(r->tool_name, sizeof(r->tool_name), "%s", tool ? tool : "");
    snprintf(r->text, sizeof(r->text), "%s", text ? text : "");
}

static void ev_cb(const aegis_agent_event_t* ev, void* user)
{
    ev_log_t* log = (ev_log_t*)user;
    switch (ev->type) {
    case AEGIS_AGENT_EVENT_TEXT_DELTA:
        ev_push(log, ev->type, NULL, AEGIS_OK, (const char*)ev->data);
        break;
    case AEGIS_AGENT_EVENT_REASONING_DELTA:
        ev_push(log, ev->type, NULL, AEGIS_OK, (const char*)ev->data);
        break;
    case AEGIS_AGENT_EVENT_TOOL_START:
    case AEGIS_AGENT_EVENT_TOOL_END:
        ev_push(log, ev->type, ev->tool_name, ev->status, NULL);
        break;
    default:
        break;
    }
}

/* ── Fixtures (mirrors tests/system/test_coding_loop.c) ───────────────── */

static int read_calls = 0;

static aegis_status_t read_probe(void* user, const aegis_tool_args_t* args,
                                 const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    ++read_calls;
    (void)token;
    const aegis_tool_value_t* value = NULL;
    assert(aegis_tool_args_find(args, "path", &value));
    assert(value->type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(value->as.str.ptr, "README.md") == 0);
    return aegis_tool_result_set_string(out, "fixture contents");
}

static aegis_tool_approval_t approve_all(const char* n, const char* a, void* u)
{
    (void)n;
    (void)a;
    (void)u;
    return AEGIS_TOOL_APPROVAL_ALLOW;
}

static aegis_tool_approval_t deny_all(const char* n, const char* a, void* u)
{
    (void)n;
    (void)a;
    (void)u;
    return AEGIS_TOOL_APPROVAL_DENY;
}

static aegis_status_t model_backend_stream(void* user, const aegis_model_request_t* request,
                                           const aegis_cancellation_token_t* token,
                                           aegis_model_stream_callback_fn    callback,
                                           void*                             callback_user)
{
    int* turn = user;
    (void)token;
    (void)request;
    if (*turn == 0) {
        ++*turn;
        aegis_model_stream_event_t start = {
            .type = AEGIS_MODEL_STREAM_TOOL_CALL_START, .index = 0,
            .tool_name = "read", .call_id = "call-1",
        };
        aegis_model_stream_event_t delta = {
            .type = AEGIS_MODEL_STREAM_TOOL_CALL_DELTA,
            .data = "{\"path\":\"README.md\"}", .len = strlen("{\"path\":\"README.md\"}"),
            .index = 0, .tool_name = "read", .call_id = "call-1",
        };
        aegis_model_stream_event_t end = {
            .type = AEGIS_MODEL_STREAM_TOOL_CALL_END, .index = 0,
            .tool_name = "read", .call_id = "call-1",
        };
        assert(callback(&start, callback_user) == AEGIS_OK);
        assert(callback(&delta, callback_user) == AEGIS_OK);
        assert(callback(&end, callback_user) == AEGIS_OK);
        aegis_usage_t              u  = {.input_tokens = 11, .output_tokens = 7, .total_tokens = 18};
        aegis_model_stream_event_t uu = {
            .type = AEGIS_MODEL_STREAM_USAGE, .data = &u, .len = sizeof(u)};
        assert(callback(&uu, callback_user) == AEGIS_OK);
    } else {
        const char*                r     = "thinking hard";
        aegis_model_stream_event_t rev   = {
            .type = AEGIS_MODEL_STREAM_REASONING_DELTA, .data = r, .len = strlen(r),
        };
        assert(callback(&rev, callback_user) == AEGIS_OK);
        const char*                text  = "done";
        aegis_model_stream_event_t event = {
            .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = text, .len = strlen(text),
        };
        assert(callback(&event, callback_user) == AEGIS_OK);
        /* Providers report usage near the end of the stream. */
        aegis_model_stream_event_t usage = {
            .type = AEGIS_MODEL_STREAM_USAGE, .data = NULL, .len = sizeof(aegis_usage_t),
        };
        aegis_usage_t u = {.input_tokens = 11, .output_tokens = 7, .total_tokens = 18};
        usage.data       = &u;
        assert(callback(&usage, callback_user) == AEGIS_OK);
    }
    aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END};
    return callback(&end, callback_user);
}

/* Simulates a provider that observes cancellation mid-stream: emits one
 * text delta then returns CANCELLED (without STREAM_END). */
static aegis_status_t cancel_mid_stream(void* user, const aegis_model_request_t* request,
                                        const aegis_cancellation_token_t* token,
                                        aegis_model_stream_callback_fn    callback,
                                        void*                             callback_user)
{
    (void)user;
    (void)request;
    (void)token;
    const char*                part = "partial text";
    aegis_model_stream_event_t ev   = {
        .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = part, .len = strlen(part),
    };
    assert(callback(&ev, callback_user) == AEGIS_OK);
    return AEGIS_ERR_CANCELLED;
}

int main(void)
{
    aegis_session_t* session = NULL;
    assert(aegis_session_create(".", &session) == AEGIS_OK);
    aegis_tool_registry_t* tools = NULL;
    assert(aegis_tool_registry_create(&tools) == AEGIS_OK);
    static const aegis_tool_param_spec_t params[] = {
        {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    };
    aegis_tool_def_t def = {
        .name    = "read",
        .schema  = {.params = params, .param_count = 1},
        .execute = read_probe,
    };
    assert(aegis_tool_registry_register(tools, &def) == AEGIS_OK);
    int                   turn    = 0;
    aegis_model_backend_t backend = {
        .user     = &turn,
        .complete = NULL,
        .stream   = model_backend_stream,
        .capabilities =
            AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_TOOL_CALLING | AEGIS_MODEL_CAP_STREAMING,
    };
    aegis_model_client_t* model = NULL;
    assert(aegis_model_client_create_with_backend("fixture", &backend, &model) == AEGIS_OK);

    ev_log_t log = {0};
    aegis_agent_loop_config_t config = {
        .session       = session,
        .model         = model,
        .tools         = tools,
        .system_prompt = "fixture",
        .on_event      = ev_cb,
        .event_user    = &log,
    };
    aegis_agent_loop_t* loop = NULL;
    assert(aegis_agent_loop_create(&config, &loop) == AEGIS_OK);

    assert(aegis_agent_loop_run_turn(loop, "read README.md") == AEGIS_OK);

    /* Event sequence: TOOL_START(read) -> TOOL_END(read, ok) -> REASONING_DELTA -> TEXT_DELTA */
    assert(log.n == 4);
    assert(log.items[0].type == AEGIS_AGENT_EVENT_TOOL_START);
    assert(strcmp(log.items[0].tool_name, "read") == 0);
    assert(log.items[1].type == AEGIS_AGENT_EVENT_TOOL_END);
    assert(strcmp(log.items[1].tool_name, "read") == 0);
    assert(log.items[1].status == AEGIS_OK);
    assert(log.items[2].type == AEGIS_AGENT_EVENT_REASONING_DELTA);
    assert(strcmp(log.items[2].text, "thinking hard") == 0);
    assert(log.items[3].type == AEGIS_AGENT_EVENT_TEXT_DELTA);
    assert(strcmp(log.items[3].text, "done") == 0);

    /* Reasoning attached to the final assistant message in the session. */
    const aegis_message_t* last_msg =
        aegis_session_message_at(session, aegis_session_message_count(session) - 1);
    assert(aegis_message_reasoning(last_msg) != NULL);
    assert(strcmp(aegis_message_reasoning(last_msg), "thinking hard") == 0);

    /* Runtime rebinding to NULL: no further events recorded */
    log.n = 0;
    assert(aegis_agent_loop_set_event_callback(loop, NULL, NULL) == AEGIS_OK);
    turn = 0; /* reset fixture backend */
    assert(aegis_agent_loop_run_turn(loop, "read README.md again") == AEGIS_OK);
    assert(log.n == 0);

    /* Invalid args */
    assert(aegis_agent_loop_set_event_callback(NULL, ev_cb, &log) == AEGIS_ERR_INVALID);

    /* ── Tool approval hook ───────────────────────────────────────────── */
    /* Deny-all: probe must NOT run; denial text goes back as tool result. */
    read_calls = 0;
    turn = 0;
    assert(aegis_agent_loop_set_tool_approval(loop, deny_all, NULL) == AEGIS_OK);
    assert(aegis_agent_loop_run_turn(loop, "read README.md again") == AEGIS_OK);
    assert(read_calls == 0);
    size_t nm = aegis_session_message_count(session);
    const aegis_message_t* tr_msg = NULL;
    for (size_t i = nm; i > 0; i--) {
        const aegis_message_t* m = aegis_session_message_at(session, i - 1);
        if (aegis_message_role(m) == AEGIS_MESSAGE_TOOL) {
            tr_msg = m;
            break;
        }
    }
    assert(tr_msg != NULL);
    assert(strstr(aegis_message_content(tr_msg), "user denied tool read") != NULL);

    /* Allow-all: executes normally. */
    read_calls = 0;
    turn = 0;
    assert(aegis_agent_loop_set_tool_approval(loop, approve_all, NULL) == AEGIS_OK);
    assert(aegis_agent_loop_run_turn(loop, "read README.md once more") == AEGIS_OK);
    assert(read_calls == 1);

    /* Unset the gate: back to implicit allow. */
    read_calls = 0;
    turn = 0;
    assert(aegis_agent_loop_set_tool_approval(loop, NULL, NULL) == AEGIS_OK);
    assert(aegis_agent_loop_run_turn(loop, "read README.md yet again") == AEGIS_OK);
    assert(read_calls == 1);

    assert(aegis_agent_loop_set_tool_approval(NULL, approve_all, NULL) == AEGIS_ERR_INVALID);

    /* ── Usage aggregation: last turn + lifetime totals ─────────────────── */
    {
        aegis_usage_t last = {0}, total = {0};
        assert(aegis_agent_loop_last_usage(loop, &last) == AEGIS_OK);
        assert(aegis_agent_loop_usage(loop, &total) == AEGIS_OK);
        /* Five run_turns each made two model rounds (tool call + reply);
         * every round reports 11/7/18. Last round = the final reply. */
        assert(last.input_tokens == 11 && last.output_tokens == 7 && last.total_tokens == 18);
        assert(total.input_tokens == 110 && total.output_tokens == 70 && total.total_tokens == 180);
        assert(aegis_agent_loop_usage(NULL, &total) == AEGIS_ERR_INVALID);
        assert(aegis_agent_loop_usage(loop, NULL) == AEGIS_ERR_INVALID);
    }

    /* ── Cancellation: partial reply is preserved + marker appended ────── */
    {
        int                   turn2    = 0;
        aegis_model_backend_t cbackend = {
            .user = &turn2, .complete = NULL, .stream = cancel_mid_stream,
            .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_STREAMING,
        };
        aegis_model_client_t* cmodel = NULL;
        assert(aegis_model_client_create_with_backend("fixture-cancel", &cbackend, &cmodel) ==
               AEGIS_OK);
        aegis_agent_loop_config_t ccfg = {
            .session = session, .model = cmodel, .tools = tools, .system_prompt = "fixture",
        };
        aegis_agent_loop_t* cloop = NULL;
        assert(aegis_agent_loop_create(&ccfg, &cloop) == AEGIS_OK);

        size_t before = aegis_session_message_count(session);
        assert(aegis_agent_loop_run_turn(cloop, "interrupt me") == AEGIS_ERR_CANCELLED);
        assert(aegis_agent_loop_state(cloop) == AEGIS_AGENT_LOOP_CANCELLED);

        /* session gained: user msg, partial assistant msg, marker user msg */
        assert(aegis_session_message_count(session) == before + 3);
        const aegis_message_t* amsg = aegis_session_message_at(session, before + 1);
        assert(aegis_message_role(amsg) == AEGIS_MESSAGE_ASSISTANT);
        assert(strcmp(aegis_message_content(amsg), "partial text") == 0);
        const aegis_message_t* marker = aegis_session_message_at(session, before + 2);
        assert(aegis_message_role(marker) == AEGIS_MESSAGE_USER);
        assert(strcmp(aegis_message_content(marker), "[interrupted by user]") == 0);

        /* Token rebind: set_token validates and stores. */
        aegis_cancellation_token_t* tok = NULL;
        assert(aegis_cancellation_token_create(&tok) == AEGIS_OK);
        assert(aegis_agent_loop_set_token(cloop, tok) == AEGIS_OK);
        assert(aegis_agent_loop_set_token(NULL, tok) == AEGIS_ERR_INVALID);
        aegis_cancellation_token_request_cancel(tok);
        /* run_turn refuses immediately on a cancelled token (pre-existing guard) */
        assert(aegis_agent_loop_run_turn(cloop, "x") == AEGIS_ERR_CANCELLED);
        aegis_cancellation_token_destroy(tok);

        aegis_agent_loop_destroy(cloop);
        aegis_model_client_destroy(cmodel);
    }

    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_tool_registry_destroy(tools);
    aegis_session_destroy(session);
    puts("ALL_AGENT_EVENT_TESTS PASSED");
    return 0;
}
