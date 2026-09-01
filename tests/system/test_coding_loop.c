#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
#include "aegis/tool/tool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int calls;

static aegis_status_t read_probe(void* user, const aegis_tool_args_t* args,
                                 const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* value = NULL;
    assert(aegis_tool_args_find(args, "path", &value));
    assert(value->type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(value->as.str.ptr, "README.md") == 0);
    ++calls;
    return aegis_tool_result_set_string(out, "fixture contents");
}

static aegis_status_t model_backend_stream(void* user, const aegis_model_request_t* request,
                                           const aegis_cancellation_token_t* token,
                                           aegis_model_stream_callback_fn    callback,
                                           void*                             callback_user)
{
    int* turn = user;
    (void)token;
    if (*turn == 0) {
        ++*turn;
        aegis_model_stream_event_t start = {
            .type      = AEGIS_MODEL_STREAM_TOOL_CALL_START,
            .index     = 0,
            .tool_name = "read",
            .call_id   = "call-1",
        };
        aegis_model_stream_event_t delta = {
            .type      = AEGIS_MODEL_STREAM_TOOL_CALL_DELTA,
            .data      = "{\"path\":\"README.md\"}",
            .len       = strlen("{\"path\":\"README.md\"}"),
            .index     = 0,
            .tool_name = "read",
            .call_id   = "call-1",
        };
        aegis_model_stream_event_t end = {
            .type      = AEGIS_MODEL_STREAM_TOOL_CALL_END,
            .index     = 0,
            .tool_name = "read",
            .call_id   = "call-1",
        };
        assert(callback(&start, callback_user) == AEGIS_OK);
        assert(callback(&delta, callback_user) == AEGIS_OK);
        assert(callback(&end, callback_user) == AEGIS_OK);
    } else {
        assert(request->messages != NULL);
        size_t n = aegis_message_list_count(request->messages);
        assert(n >= 3);
        const aegis_message_t* tool = aegis_message_list_at(request->messages, n - 1);
        assert(aegis_message_role(tool) == AEGIS_MESSAGE_TOOL);
        assert(strcmp(aegis_message_tool_call_id(tool), "call-1") == 0);
        assert(strcmp(aegis_message_content(tool), "fixture contents") == 0);
        const char*                text  = "done";
        aegis_model_stream_event_t event = {
            .type = AEGIS_MODEL_STREAM_TEXT_DELTA,
            .data = text,
            .len  = strlen(text),
        };
        assert(callback(&event, callback_user) == AEGIS_OK);
    }
    aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END};
    return callback(&end, callback_user);
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
    aegis_agent_loop_config_t config = {
        .session       = session,
        .model         = model,
        .tools         = tools,
        .system_prompt = "fixture",
    };
    aegis_agent_loop_t* loop = NULL;
    assert(aegis_agent_loop_create(&config, &loop) == AEGIS_OK);
    aegis_status_t run_status = aegis_agent_loop_run_turn(loop, "read README.md");
    assert(run_status == AEGIS_OK);
    assert(calls == 1);
    assert(aegis_session_message_count(session) == 4);
    const aegis_message_t* final = aegis_session_message_at(session, 3);
    assert(strcmp(aegis_message_content(final), "done") == 0);
    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_tool_registry_destroy(tools);
    aegis_session_destroy(session);
    puts("coding_loop: PASS");
    return 0;
}
