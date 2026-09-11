/**
 * @file test_loop_strategy.c
 * @brief Unit tests for loop-strategy hooks: ordering, abort propagation,
 * should_continue cap, ABI rejection, tool-validation errors with last-error
 * detail, and context-truncation reporting.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/common/error.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
#include "aegis/status.h"
#include "aegis/tool/tool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct strat_rec {
    int            before;
    int            after_model;
    int            after_tool;
    int            cont_calls;
    int            cont_ret;
    aegis_status_t before_rc;
    int            order[8];
    int            order_n;
} strat_rec_t;

static void rec_push(strat_rec_t* r, int v)
{
    if (r->order_n < 8) {
        r->order[r->order_n++] = v;
    }
}

static aegis_status_t rec_init(void* user)
{
    (void)user;
    return AEGIS_OK;
}

static aegis_status_t rec_shutdown(void* user)
{
    (void)user;
    return AEGIS_OK;
}

static aegis_status_t rec_before(void* user, aegis_agent_loop_t* loop)
{
    strat_rec_t* r = (strat_rec_t*)user;
    (void)loop;
    r->before++;
    rec_push(r, 1);
    return r->before_rc;
}

static aegis_status_t rec_after_model(void* user, aegis_agent_loop_t* loop)
{
    strat_rec_t* r = (strat_rec_t*)user;
    (void)loop;
    r->after_model++;
    rec_push(r, 2);
    return AEGIS_OK;
}

static aegis_status_t rec_after_tool(void* user, aegis_agent_loop_t* loop)
{
    strat_rec_t* r = (strat_rec_t*)user;
    (void)loop;
    r->after_tool++;
    return AEGIS_OK;
}

static aegis_status_t rec_continue(void* user, int* out)
{
    strat_rec_t* r = (strat_rec_t*)user;
    r->cont_calls++;
    *out = r->cont_ret;
    return AEGIS_OK;
}

static aegis_status_t text_backend_stream(void* user, const aegis_model_request_t* req,
                                          const aegis_cancellation_token_t* tok,
                                          aegis_model_stream_callback_fn cb, void* cbuser)
{
    const char* text = "hi";
    (void)user;
    (void)req;
    (void)tok;
    aegis_model_stream_event_t ev = {
        .type = AEGIS_MODEL_STREAM_TEXT_DELTA,
        .data = text,
        .len  = strlen(text),
    };
    if (cb(&ev, cbuser) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END};
    return cb(&end, cbuser);
}

static aegis_model_backend_t text_backend = {
    .user         = NULL,
    .complete     = NULL,
    .stream       = text_backend_stream,
    .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_STREAMING,
};

typedef struct loop_ctx {
    aegis_session_t*      session;
    aegis_model_client_t* model;
    aegis_agent_loop_t*   loop;
} loop_ctx_t;

static void ctx_destroy(loop_ctx_t* c)
{
    if (c->loop) {
        aegis_agent_loop_destroy(c->loop);
        c->loop = NULL;
    }
    if (c->model) {
        aegis_model_client_destroy(c->model);
        c->model = NULL;
    }
    if (c->session) {
        aegis_session_destroy(c->session);
        c->session = NULL;
    }
}

static aegis_status_t ctx_create(loop_ctx_t* c, const aegis_agent_strategy_def_t* def,
                                 uint32_t max_turns)
{
    memset(c, 0, sizeof(*c));
    if (aegis_session_create(".", &c->session) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    if (aegis_model_client_create_with_backend("fixture-strategy", &text_backend, &c->model) !=
        AEGIS_OK) {
        ctx_destroy(c);
        return AEGIS_ERR_INTERNAL;
    }
    aegis_agent_loop_config_t cfg = {
        .session            = c->session,
        .model              = c->model,
        .tools              = NULL,
        .system_prompt      = NULL,
        .token              = NULL,
        .on_event           = NULL,
        .event_user         = NULL,
        .tool_approval      = NULL,
        .approval_user      = NULL,
        .strategy           = def,
        .max_strategy_turns = max_turns,
    };
    aegis_status_t st = aegis_agent_loop_create(&cfg, &c->loop);
    if (st != AEGIS_OK) {
        ctx_destroy(c);
        return st;
    }
    return AEGIS_OK;
}

static void make_def(strat_rec_t* r, aegis_agent_strategy_def_t* def)
{
    memset(r, 0, sizeof(*r));
    memset(def, 0, sizeof(*def));
    def->name            = "rec";
    def->description     = "test recorder";
    def->abi_version     = AEGIS_AGENT_STRATEGY_ABI_VERSION;
    def->init            = rec_init;
    def->before_turn     = rec_before;
    def->after_model     = rec_after_model;
    def->after_tool      = rec_after_tool;
    def->should_continue = rec_continue;
    def->shutdown        = rec_shutdown;
    def->user            = r;
}

static aegis_status_t badargs_backend_stream(void* user, const aegis_model_request_t* req,
                                               const aegis_cancellation_token_t* tok,
                                               aegis_model_stream_callback_fn cb, void* cbuser)
{
    const char* bad = "not-json{{{";
    (void)user;
    (void)req;
    (void)tok;
    aegis_model_stream_event_t start = {
        .type = AEGIS_MODEL_STREAM_TOOL_CALL_START, .index = 0, .tool_name = "read",
        .call_id = "call-9",
    };
    aegis_model_stream_event_t delta = {
        .type = AEGIS_MODEL_STREAM_TOOL_CALL_DELTA, .data = bad, .len = strlen(bad), .index = 0,
        .tool_name = "read", .call_id = "call-9",
    };
    aegis_model_stream_event_t end = {
        .type = AEGIS_MODEL_STREAM_TOOL_CALL_END, .index = 0, .tool_name = "read",
        .call_id = "call-9",
    };
    if (cb(&start, cbuser) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    if (cb(&delta, cbuser) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    if (cb(&end, cbuser) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    aegis_model_stream_event_t done = {.type = AEGIS_MODEL_STREAM_END};
    return cb(&done, cbuser);
}

static aegis_status_t dummy_execute(void* user, const aegis_tool_args_t* args,
                                    const aegis_cancellation_token_t* token,
                                    aegis_tool_result_t*              out)
{
    (void)user;
    (void)args;
    (void)token;
    (void)out;
    return AEGIS_OK;
}

int main(void)
{
    /* ABI mismatch is rejected at create. */
    {
        strat_rec_t                r;
        aegis_agent_strategy_def_t def;
        loop_ctx_t                 c;
        make_def(&r, &def);
        def.abi_version = 0;
        assert(ctx_create(&c, &def, 0) == AEGIS_ERR_INVALID);
        ctx_destroy(&c);
        printf("abi_mismatch PASS\n");
    }

    /* Hook order with a text-only turn: before, after_model, continue. */
    {
        strat_rec_t                r;
        aegis_agent_strategy_def_t def;
        loop_ctx_t                 c;
        make_def(&r, &def);
        r.cont_ret = 0;
        assert(ctx_create(&c, &def, 0) == AEGIS_OK);
        assert(aegis_agent_loop_run_turn(c.loop, "hello") == AEGIS_OK);
        assert(r.before == 1);
        assert(r.after_model == 1);
        assert(r.after_tool == 0);
        assert(r.cont_calls == 1);
        assert(r.order_n == 2 && r.order[0] == 1 && r.order[1] == 2);
        ctx_destroy(&c);
        printf("hook_order PASS\n");
    }

    /* should_continue cap: 1 initial + max continued turns. */
    {
        strat_rec_t                r;
        aegis_agent_strategy_def_t def;
        loop_ctx_t                 c;
        make_def(&r, &def);
        r.cont_ret = 1;
        assert(ctx_create(&c, &def, 2) == AEGIS_OK);
        assert(aegis_agent_loop_run_turn(c.loop, "hello") == AEGIS_OK);
        assert(r.before == 1);
        assert(r.after_model == 3);
        assert(r.cont_calls == 3);
        ctx_destroy(&c);
        printf("continue_cap PASS\n");
    }

    /* Hook failure aborts the turn. */
    {
        strat_rec_t                r;
        aegis_agent_strategy_def_t def;
        loop_ctx_t                 c;
        make_def(&r, &def);
        r.before_rc = AEGIS_ERR_INVALID;
        assert(ctx_create(&c, &def, 0) == AEGIS_OK);
        assert(aegis_agent_loop_run_turn(c.loop, "hello") == AEGIS_ERR_INVALID);
        assert(r.before == 1);
        assert(r.after_model == 0);
        ctx_destroy(&c);
        printf("hook_abort PASS\n");
    }

    {
        aegis_session_t* session = NULL;
        assert(aegis_session_create(".", &session) == AEGIS_OK);
        aegis_tool_registry_t* tools = NULL;
        assert(aegis_tool_registry_create(&tools) == AEGIS_OK);
        static const aegis_tool_param_spec_t params[] = {
            {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = true},
        };
        aegis_tool_def_t tdef = {
            .name     = "read",
            .schema   = {.params = params, .param_count = 1},
            .execute  = dummy_execute,
        };
        assert(aegis_tool_registry_register(tools, &tdef) == AEGIS_OK);
        static aegis_model_backend_t bad_backend = {
            .user         = NULL,
            .complete     = NULL,
            .stream       = badargs_backend_stream,
            .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_TOOL_CALLING |
                            AEGIS_MODEL_CAP_STREAMING,
        };
        aegis_model_client_t* model = NULL;
        assert(aegis_model_client_create_with_backend("fixture-badargs", &bad_backend, &model) ==
               AEGIS_OK);
        aegis_agent_loop_t*       loop = NULL;
        aegis_agent_loop_config_t cfg  = {
               .session = session,
               .model   = model,
               .tools   = tools,
        };
        assert(aegis_agent_loop_create(&cfg, &loop) == AEGIS_OK);
        assert(aegis_agent_loop_run_turn(loop, "hi") == AEGIS_ERR_TOOL_VALIDATION);
        assert(strcmp(aegis_status_str(AEGIS_ERR_TOOL_VALIDATION), "tool_validation") == 0);
        assert(strcmp(aegis_status_str(AEGIS_ERR_CONTEXT_OVERFLOW), "context_overflow") == 0);
        assert(strcmp(aegis_status_str(AEGIS_ERR_MODEL_RATE_LIMIT), "model_rate_limit") == 0);
        const aegis_error_t* detail = aegis_error_last();
        assert(detail != NULL);
        assert(aegis_error_code(detail) == AEGIS_ERROR_FORMAT);
        assert(strstr(aegis_error_message(detail), "read") != NULL);
        aegis_error_set_last(NULL);
        assert(aegis_error_last() == NULL);
        aegis_agent_loop_destroy(loop);
        aegis_model_client_destroy(model);
        aegis_tool_registry_destroy(tools);
        aegis_session_destroy(session);
        printf("tool_validation PASS\n");
    }

    {
        loop_ctx_t c;
        assert(ctx_create(&c, NULL, 0) == AEGIS_OK);
        assert(aegis_agent_loop_context_dropped(c.loop) == 0);
        for (int i = 0; i < 130; i++) {
            aegis_message_t* m = NULL;
            assert(aegis_message_create(AEGIS_MESSAGE_USER, &m) == AEGIS_OK);
            assert(aegis_message_set_content(m, "filler") == AEGIS_OK);
            assert(aegis_session_append_message(c.session, m) == AEGIS_OK);
            aegis_message_destroy(m);
        }
        assert(aegis_agent_loop_run_turn(c.loop, "go") == AEGIS_OK);
        assert(aegis_agent_loop_context_dropped(c.loop) == 3);
        ctx_destroy(&c);
        printf("context_dropped PASS\n");
    }

    printf("ALL_LOOP_STRATEGY_TESTS PASSED\n");
    return 0;
}
