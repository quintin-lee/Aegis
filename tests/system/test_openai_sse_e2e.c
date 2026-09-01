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
    fixture->saw_auth   = strstr(request, "Authorization: Bearer test-key") != NULL;
    fixture->saw_stream = strstr(request, "stream") != NULL;

    const char* body = fixture->status_code == 200
                           ? "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
                             "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                             "data: "
                             "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-"
                             "1\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":"
                             "\\\"README.md\\\"}\"}}]}}]}\n\n"          "data: [DONE]\n\n"
        : "{\"error\":{\"message\":\"bad request\"}}";
    if (fixture->close_without_done) body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";

    char        header[256];
    int         header_len = snprintf(
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
} events_t;

static aegis_status_t collect_event(const aegis_model_stream_event_t* event, void* user)
{
    events_t* events = user;
    if (event->type == AEGIS_MODEL_STREAM_TEXT_DELTA) {
        assert(events->text_len + event->len < sizeof(events->text));
        memcpy(events->text + events->text_len, event->data, event->len);
        events->text_len += event->len;
        events->text[events->text_len] = '\0';
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
    aegis_model_request_t request = {.model = "test-model", .messages = messages, .stream = true};
    events_t              events  = {0};
    assert(backend.stream(backend.user, &request, NULL, collect_event, &events) == AEGIS_OK);
    assert(strcmp(events.text, "Hello") == 0);
    assert(events.starts == 1 && events.deltas == 1 && events.ends == 1);
    assert(strcmp(events.name, "read") == 0);
    assert(strcmp(events.id, "call-1") == 0);
    assert(strcmp(events.args, "{\"path\":\"README.md\"}") == 0);
    assert(fixture.saw_auth && fixture.saw_stream);
    pthread_join(thread, NULL);
    close(fixture.server_fd);

    fixture_t incomplete_fixture = {.status_code = 200, .close_without_done = 1};
    port = start_fixture(&incomplete_fixture);
    assert(pthread_create(&thread, NULL, fixture_thread, &incomplete_fixture) == 0);
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    aegis_openai_model_ctx_t* incomplete_context = NULL;
    aegis_model_backend_t incomplete_backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &incomplete_context, &incomplete_backend) == AEGIS_OK);
    assert(incomplete_backend.stream(incomplete_backend.user, &request, NULL, collect_event, &events) == AEGIS_ERR_PROVIDER);
    pthread_join(thread, NULL);
    close(incomplete_fixture.server_fd);
    aegis_openai_model_destroy(incomplete_context);

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

    aegis_message_destroy(user);
    aegis_message_list_destroy(messages);
    aegis_openai_model_destroy(context);
    puts("openai_sse_e2e: PASS");
    return 0;
}
