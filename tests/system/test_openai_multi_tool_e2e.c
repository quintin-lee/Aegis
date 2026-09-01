#define _POSIX_C_SOURCE 200809L
#include "structured_openai.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { int fd; } fixture_t;

static void* server_thread(void* user)
{
    fixture_t* fixture = user;
    int client = accept(fixture->fd, NULL, NULL);
    assert(client >= 0);
    char request[4096];
    assert(recv(client, request, sizeof(request), 0) > 0);
    const char* body =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call-b\",\"function\":{\"name\":\"write\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-a\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"function\":{\"arguments\":\"out.txt\\\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"README.md\\\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    char header[256];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
    assert(send(client, header, (size_t)n, 0) == n);
    assert(send(client, body, strlen(body), 0) == (ssize_t)strlen(body));
    close(client);
    return NULL;
}

static int make_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(fd, 1) == 0);
    return fd;
}

typedef struct {
    int starts;
    int deltas;
    int ends;
    uint32_t indexes[4];
    char names[4][16];
    char ids[4][16];
    char args[4][64];
} events_t;

static aegis_status_t collect(const aegis_model_stream_event_t* event, void* user)
{
    events_t* events = user;
    if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_START) {
        int i = events->starts++;
        assert(i < 4);
        events->indexes[i] = event->index;
        snprintf(events->names[i], sizeof(events->names[i]), "%s", event->tool_name);
        snprintf(events->ids[i], sizeof(events->ids[i]), "%s", event->call_id);
    } else if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_DELTA) {
        int i = event->index;
        assert(i < 4);
        ++events->deltas;
        size_t current = strlen(events->args[i]);
        assert(current + event->len < sizeof(events->args[i]));
        memcpy(events->args[i] + current, event->data, event->len);
        events->args[i][current + event->len] = '\0';
    } else if (event->type == AEGIS_MODEL_STREAM_TOOL_CALL_END) {
        ++events->ends;
    }
    return AEGIS_OK;
}

int main(void)
{
    int fd = make_server();
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    assert(getsockname(fd, (struct sockaddr*)&address, &length) == 0);
    fixture_t fixture = {.fd = fd};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, &fixture) == 0);
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%u/v1", ntohs(address.sin_port));
    aegis_openai_model_ctx_t* context = NULL;
    aegis_model_backend_t backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &context, &backend) == AEGIS_OK);
    aegis_message_list_t* messages = NULL;
    assert(aegis_message_list_create(&messages) == AEGIS_OK);
    aegis_message_t* user = NULL;
    assert(aegis_message_create(AEGIS_MESSAGE_USER, &user) == AEGIS_OK);
    assert(aegis_message_set_content(user, "do both") == AEGIS_OK);
    assert(aegis_message_list_append(messages, user) == AEGIS_OK);
    aegis_model_request_t request = {.model = "test-model", .messages = messages, .stream = true};
    events_t events = {0};
    assert(backend.stream(backend.user, &request, NULL, collect, &events) == AEGIS_OK);
    assert(events.starts == 2 && events.deltas == 4 && events.ends == 2);
    assert(events.indexes[0] == 1 && events.indexes[1] == 0);
    assert(strcmp(events.names[0], "write") == 0 && strcmp(events.names[1], "read") == 0);
    assert(strcmp(events.args[0], "{\"path\":") == 0);
    assert(strcmp(events.args[1], "{\"path\":") == 0);
    pthread_join(thread, NULL);
    close(fd);
    aegis_message_destroy(user);
    aegis_message_list_destroy(messages);
    aegis_openai_model_destroy(context);
    puts("openai_multi_tool_e2e: PASS");
    return 0;
}
