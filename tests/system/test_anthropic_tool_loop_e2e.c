/**
 * @file test_anthropic_tool_loop_e2e.c
 * @brief End-to-end: full agent-loop tool round-trip against the Anthropic
 * provider backend.
 *
 * A local mock server answers the first POST /v1/messages with an SSE
 * tool_use block (read README.md) and the second with a text answer. The
 * agent loop must execute the tool and finish with "finished".
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/common/error.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
#include "aegis/tool/tool.h"
#include "structured_anthropic.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int listen_fd;
    int port;
    int requests;
} server_t;

/* Tool executor: the loop calls it with a "path" argument; assert + return. */
static aegis_status_t fixture_read(void* user, const aegis_tool_args_t* args,
                                   const aegis_cancellation_token_t* token,
                                   aegis_tool_result_t*              out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* value = NULL;
    assert(aegis_tool_args_find(args, "path", &value));
    assert(value && value->type == AEGIS_TOOL_VAL_STRING);
    return aegis_tool_result_set_string(out, "fixture file contents");
}

static void* server_thread(void* arg)
{
    server_t* s = arg;
    char      buf[65536];
    for (int i = 0; i < 2; ++i) {
        struct sockaddr_in cli;
        socklen_t          clilen = sizeof(cli);
        int                fd     = accept(s->listen_fd, (struct sockaddr*)&cli, &clilen);
        if (fd < 0) {
            break;
        }
        int total = 0;
        while (total < (int)sizeof(buf)) {
            int n = read(fd, buf + total, sizeof(buf) - total);
            if (n <= 0) {
                break;
            }
            total += n;
            if (strstr(buf, "\r\n\r\n")) {
                break;
            }
        }
        s->requests++;
        const char* body;
        if (i == 0) {
            body =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                "event: message_start\ndata: "
                "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":5}}}\n\n"
                "event: content_block_start\ndata: "
                "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_"
                "use\",\"id\":\"toolu_1\",\"name\":\"read\"}}\n\n"
                "event: content_block_delta\ndata: "
                "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_"
                "delta\",\"partial_json\":\"{\\\"path\\\":\\\"README.md\\\"}\"}}\n\n"
                "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\ndata: "
                "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{"
                "\"output_tokens\":7}}\n\n"
                "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        } else {
            body =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                "event: message_start\ndata: "
                "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":9}}}\n\n"
                "event: content_block_start\ndata: "
                "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
                "\"text\"}}\n\n"
                "event: content_block_delta\ndata: "
                "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
                "\"text\":\"finished\"}}\n\n"
                "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\ndata: "
                "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{"
                "\"output_tokens\":3}}\n\n"
                "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        }
        (void)write(fd, body, strlen(body));
        close(fd);
    }
    return NULL;
}

static int make_server(int* listen_fd, int* port)
{
    *listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*listen_fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(*listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = 0;
    if (bind(*listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        return -1;
    }
    if (listen(*listen_fd, 8) < 0) {
        return -1;
    }
    struct sockaddr_in bound;
    socklen_t          blen = sizeof(bound);
    getsockname(*listen_fd, (struct sockaddr*)&bound, &blen);
    *port = ntohs(bound.sin_port);
    return 0;
}

int main(void)
{
    int listen_fd = -1, port = 0;
    assert(make_server(&listen_fd, &port) == 0);
    server_t  srv = {.listen_fd = listen_fd};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, &srv) == 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    aegis_anthropic_model_ctx_t* ctx     = NULL;
    aegis_model_backend_t        backend = {0};
    aegis_status_t               st =
        aegis_anthropic_model_create("test-key", base_url, "test-model", &ctx, &backend);
    assert(st == AEGIS_OK);

    aegis_session_t* session = NULL;
    assert(aegis_session_create(".", &session) == AEGIS_OK);

    aegis_tool_registry_t* tools = NULL;
    assert(aegis_tool_registry_create(&tools) == AEGIS_OK);
    static const aegis_tool_param_spec_t params[] = {
        {.name        = "path",
         .type        = AEGIS_TOOL_VAL_STRING,
         .required    = true,
         .description = "file path"},
    };
    aegis_tool_def_t definition = {
        .name = "read", .schema = {.params = params, .param_count = 1}, .execute = fixture_read};
    assert(aegis_tool_registry_register(tools, &definition) == AEGIS_OK);

    aegis_model_client_t* model = NULL;
    st = aegis_model_client_create_with_backend("test-model", &backend, &model);
    assert(st == AEGIS_OK);

    aegis_agent_loop_config_t cfg = {
        .session       = session,
        .model         = model,
        .tools         = tools,
        .system_prompt = "fixture",
    };
    aegis_agent_loop_t* loop = NULL;
    assert(aegis_agent_loop_create(&cfg, &loop) == AEGIS_OK);

    st = aegis_agent_loop_run_turn(loop, "inspect README.md");
    assert(st == AEGIS_OK);
    assert(srv.requests == 2);
    assert(aegis_session_message_count(session) == 4);
    aegis_message_t* last = aegis_session_message_at(session, 3);
    assert(last && strcmp(aegis_message_content(last), "finished") == 0);

    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_tool_registry_destroy(tools);
    aegis_session_destroy(session);
    aegis_anthropic_model_destroy(ctx);
    pthread_join(thread, NULL);
    close(listen_fd);
    puts("PASS");
    return 0;
}
