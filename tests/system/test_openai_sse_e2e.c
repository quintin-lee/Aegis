#define _POSIX_C_SOURCE 200809L
#include "structured_openai.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    int server_fd;
    int status_code;
    int close_without_done;
    int saw_auth;
    int saw_stream;
    int multi_chunk_tool;   /* stream a tool call split across chunks */
    int reasoning_field;    /* 0=none, 1=reasoning_content, 2=reasoning */
    const char* req_model;  /* captured "model" from request body */
    char* req_model_copy;
} fixture_t;

static void* fixture_thread(void* user)
{
    fixture_t* fixture = user;
    int        client  = accept(fixture->server_fd, NULL, NULL);
    assert(client >= 0);
    char   request[8192] = {0};
    size_t used          = 0;
    while (used + 1 < sizeof(request) && !strstr(request, "\r\n\r\n")) {
        ssize_t received = recv(client, request + used, sizeof(request) - used - 1, 0);
        assert(received > 0);
        used += (size_t)received;
        request[used] = '\0';
    }
    fixture->saw_auth = strstr(request, "Authorization: Bearer test-key") != NULL;
    fixture->saw_stream = strstr(request, "stream") != NULL;
    /* Capture the model name from the body (after the header blank line). */
    const char* body_start = strstr(request, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        const char* model_key = strstr(body_start, "\"model\":");
        if (model_key) {
            model_key += strlen("\"model\":");
            while (*model_key == ' ') ++model_key;
            if (*model_key == '"') {
                ++model_key;
                char model_buf[64] = {0};
                size_t mi = 0;
                while (*model_key && *model_key != '"' && mi + 1 < sizeof(model_buf)) {
                    model_buf[mi++] = *model_key++;
                }
                free(fixture->req_model_copy);
                fixture->req_model_copy = strdup(model_buf);
                fixture->req_model = fixture->req_model_copy;
            }
        }
    }

    const char* body;
    if (fixture->status_code != 200) {
        body = "{\"error\":{\"message\":\"bad request\"}}";
    } else if (fixture->multi_chunk_tool) {
        /* Tool call split across chunks: first chunk carries id+name with empty
         * arguments, second carries only arguments (no id/name keys). */
        body = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-mc\","
               "\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"{\\\"path\\\": \\\"f.c\\\"}\"}}]}}]}\n\n"
               "data: [DONE]\n\n";
    } else if (fixture->reasoning_field == 1) {
        body = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"thin\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"king\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
               "data: [DONE]\n\n";
    } else if (fixture->reasoning_field == 2) {
        body = "data: {\"choices\":[{\"delta\":{\"reasoning\":\"why\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
               "data: [DONE]\n\n";
    } else {
        body = "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
               "data: "
               "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-"
               "1\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":"
               "\\\"README.md\\\"}\"}}]}}]}\n\n"
               "data: [DONE]\n\n";
    }
    if (fixture->close_without_done) {
        body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    }

    char header[256];
    int  header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        fixture->status_code, fixture->status_code == 200 ? "OK" : "Bad Request",
        fixture->status_code == 200 ? "text/event-stream" : "application/json", strlen(body));
    assert(send(client, header, (size_t)header_len, 0) == header_len);
    if (fixture->status_code == 200) {
        assert(send(client, body, 12, 0) == 12);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
        assert(send(client, body + 12, strlen(body) - 12, 0) == (ssize_t)(strlen(body) - 12));
    } else {
        assert(send(client, body, strlen(body), 0) == (ssize_t)strlen(body));
    }
    close(client);
    return NULL;
}

typedef struct {
    char   text[128];
    size_t text_len;
    int    starts;
    int    deltas;
    int    ends;
    char   name[32];
    char   id[32];
    char   args[128];
    char   reasoning[128];
    size_t reasoning_len;
    int    reasoning_deltas;
} events_t;

static aegis_status_t collect_event(const aegis_model_stream_event_t* event, void* user)
{
    events_t* events = user;
    if (event->type == AEGIS_MODEL_STREAM_TEXT_DELTA) {
        assert(events->text_len + event->len < sizeof(events->text));
        memcpy(events->text + events->text_len, event->data, event->len);
        events->text_len += event->len;
        events->text[events->text_len] = '\0';
    } else if (event->type == AEGIS_MODEL_STREAM_REASONING_DELTA) {
        ++events->reasoning_deltas;
        assert(events->reasoning_len + event->len < sizeof(events->reasoning));
        memcpy(events->reasoning + events->reasoning_len, event->data, event->len);
        events->reasoning_len += event->len;
        events->reasoning[events->reasoning_len] = '\0';
    } else if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_START) {
        ++events->starts;
        snprintf(events->name, sizeof(events->name), "%s", event->tool_name);
        snprintf(events->id, sizeof(events->id), "%s", event->call_id);
        assert(event->index == 0);
    } else if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_DELTA) {
        ++events->deltas;
        assert(event->len < sizeof(events->args));
        memcpy(events->args, event->data, event->len);
        events->args[event->len] = '\0';
    } else if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_END) {
        ++events->ends;
    }
    return AEGIS_OK;
}

static int start_fixture(fixture_t* fixture)
{
    fixture->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fixture->server_fd >= 0);
    int reuse = 1;
    assert(setsockopt(fixture->server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    assert(bind(fixture->server_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(fixture->server_fd, 1) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fixture->server_fd, (struct sockaddr*)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

int main(void)
{
    fixture_t fixture = {.status_code = 200};
    int       port    = start_fixture(&fixture);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fixture_thread, &fixture) == 0);

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* context = NULL;
    aegis_model_backend_t     backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &context, &backend) ==
           AEGIS_OK);
    aegis_message_list_t* messages = NULL;
    assert(aegis_message_list_create(&messages) == AEGIS_OK);
    aegis_message_t* user = NULL;
    assert(aegis_message_create(AEGIS_MESSAGE_USER, &user) == AEGIS_OK);
    assert(aegis_message_set_content(user, "inspect") == AEGIS_OK);
    assert(aegis_message_list_append(messages, user) == AEGIS_OK);
    /* req->model left NULL: provider must fall back to the ctx model name. */
    aegis_model_request_t request = {.model = NULL, .messages = messages, .stream = true};
    events_t              events  = {0};
    assert(backend.stream(backend.user, &request, NULL, collect_event, &events) == AEGIS_OK);
    assert(strcmp(events.text, "Hello") == 0);
    assert(events.starts == 1 && events.deltas == 1 && events.ends == 1);
    assert(strcmp(events.name, "read") == 0);
    assert(strcmp(events.id, "call-1") == 0);
    assert(strcmp(events.args, "{\"path\":\"README.md\"}") == 0);
    assert(fixture.saw_auth && fixture.saw_stream);
    assert(fixture.req_model != NULL && strcmp(fixture.req_model, "test-model") == 0);
    pthread_join(thread, NULL);
    close(fixture.server_fd);
    aegis_openai_model_destroy(context);
    free(fixture.req_model_copy);

    /* Multi-chunk tool call: args-only delta must not clobber id/name. */
    fixture_t mc_fixture = {.status_code = 200, .multi_chunk_tool = 1};
    port      = start_fixture(&mc_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &mc_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* mc_context = NULL;
    aegis_model_backend_t     mc_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &mc_context,
                                     &mc_backend) == AEGIS_OK);
    events_t mc_events = {0};
    assert(mc_backend.stream(mc_backend.user, &request, NULL, collect_event, &mc_events) == AEGIS_OK);
    assert(mc_events.starts == 1 && mc_events.deltas >= 1 && mc_events.ends == 1);
    assert(strcmp(mc_events.name, "read") == 0);
    assert(strcmp(mc_events.id, "call-mc") == 0);
    assert(strcmp(mc_events.args, "{\"path\": \"f.c\"}") == 0);
    pthread_join(thread, NULL);
    close(mc_fixture.server_fd);
    aegis_openai_model_destroy(mc_context);
    free(mc_fixture.req_model_copy);

    /* Reasoning via "reasoning_content" (DeepSeek-style) split across chunks. */
    fixture_t rc_fixture = {.status_code = 200, .reasoning_field = 1};
    port      = start_fixture(&rc_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &rc_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* rc_context = NULL;
    aegis_model_backend_t     rc_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &rc_context,
                                     &rc_backend) == AEGIS_OK);
    events_t rc_events = {0};
    assert(rc_backend.stream(rc_backend.user, &request, NULL, collect_event, &rc_events) == AEGIS_OK);
    assert(rc_events.reasoning_deltas == 2);
    assert(strcmp(rc_events.reasoning, "thinking") == 0);
    assert(strcmp(rc_events.text, "Hi") == 0);
    pthread_join(thread, NULL);
    close(rc_fixture.server_fd);
    aegis_openai_model_destroy(rc_context);
    free(rc_fixture.req_model_copy);

    /* Reasoning via "reasoning" (OpenRouter-style), single chunk. */
    fixture_t r_fixture = {.status_code = 200, .reasoning_field = 2};
    port      = start_fixture(&r_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &r_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* r_context = NULL;
    aegis_model_backend_t     r_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &r_context,
                                     &r_backend) == AEGIS_OK);
    events_t r_events = {0};
    assert(r_backend.stream(r_backend.user, &request, NULL, collect_event, &r_events) == AEGIS_OK);
    assert(r_events.reasoning_deltas == 1);
    assert(strcmp(r_events.reasoning, "why") == 0);
    assert(strcmp(r_events.text, "Hi") == 0);
    pthread_join(thread, NULL);
    close(r_fixture.server_fd);
    aegis_openai_model_destroy(r_context);
    free(r_fixture.req_model_copy);

    fixture_t incomplete_fixture = {.status_code = 200, .close_without_done = 1};
    port                         = start_fixture(&incomplete_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &incomplete_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* incomplete_context = NULL;
    aegis_model_backend_t     incomplete_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &incomplete_context,
                                     &incomplete_backend) == AEGIS_OK);
    assert(incomplete_backend.stream(incomplete_backend.user, &request, NULL, collect_event,
                                     &events) == AEGIS_ERR_PROVIDER);
    pthread_join(thread, NULL);
    close(incomplete_fixture.server_fd);
    aegis_openai_model_destroy(incomplete_context);
    free(incomplete_fixture.req_model_copy);

    fixture_t error_fixture = {.status_code = 400};
    port                    = start_fixture(&error_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &error_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* error_context = NULL;
    aegis_model_backend_t     error_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &error_context,
                                     &error_backend) == AEGIS_OK);
    assert(error_backend.stream(error_backend.user, &request, NULL, collect_event, &events) ==
           AEGIS_ERR_PROVIDER);
    pthread_join(thread, NULL);
    close(error_fixture.server_fd);
    aegis_openai_model_destroy(error_context);
    free(error_fixture.req_model_copy);

    aegis_message_destroy(user);
    aegis_message_list_destroy(messages);
    puts("openai_sse_e2e: PASS");
    return 0;
}
