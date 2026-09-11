/**
 * @file test_openai_http_status_e2e.c
 * @brief E2E HTTP status mapping: mock server answers 429 then 413;
 * the loop surfaces MODEL_RATE_LIMIT and CONTEXT_OVERFLOW.
 */
#define _POSIX_C_SOURCE 200809L
#include "structured_openai.h"
#include "aegis/agent/loop.h"
#include "aegis/session/session.h"
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

typedef struct {
    int fd;
    int requests;
} server_t;

static void serve_status(int client, int code, const char* reason, const char* body)
{
    char header[256];
    int  header_len =
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                 "%zu\r\nConnection: close\r\n\r\n",
                 code, reason, strlen(body));
    assert(send(client, header, (size_t)header_len, 0) == header_len);
    assert(send(client, body, strlen(body), 0) == (ssize_t)strlen(body));
}

static void* server_thread(void* user)
{
    server_t* server = user;
    while (server->requests < 2) {
        int client = accept(server->fd, NULL, NULL);
        assert(client >= 0);
        char   request[16384] = {0};
        size_t used           = 0;
        while (used + 1 < sizeof(request) && !strstr(request, "\r\n\r\n")) {
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            assert(n > 0);
            used += (size_t)n;
            request[used] = '\0';
        }
        ++server->requests;
        if (server->requests == 1) {
            serve_status(client, 429, "Too Many Requests",
                         "{\"error\":{\"message\":\"Rate limit reached, retry "
                         "later\",\"type\":\"rate_limit\",\"code\":\"rate_limit_exceeded\"}}");
        } else {
            serve_status(client, 413, "Content Too Large",
                         "{\"error\":{\"message\":\"Maximum context length "
                         "exceeded\",\"type\":\"invalid_request_error\",\"code\":"
                         "\"context_length_exceeded\"}}");
        }
        close(client);
    }
    return NULL;
}

static int make_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int reuse = 1;
    assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(fd, 2) == 0);
    return fd;
}

int main(void)
{
    int                server_fd = make_server();
    struct sockaddr_in address;
    socklen_t          address_len = sizeof(address);
    assert(getsockname(server_fd, (struct sockaddr*)&address, &address_len) == 0);
    server_t  server = {.fd = server_fd};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, &server) == 0);

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%u/v1", ntohs(address.sin_port));
    aegis_openai_model_ctx_t* openai  = NULL;
    aegis_model_backend_t     backend = {0};
    assert(aegis_openai_model_create("test-key", base_url, "test-model", &openai, &backend) ==
           AEGIS_OK);

    aegis_session_t* session = NULL;
    assert(aegis_session_create(".", &session) == AEGIS_OK);
    aegis_model_client_t* model = NULL;
    assert(aegis_model_client_create_with_backend("test-model", &backend, &model) == AEGIS_OK);
    aegis_agent_loop_config_t config = {
        .session = session, .model = model, .tools = NULL, .system_prompt = "fixture"};
    aegis_agent_loop_t* loop = NULL;
    assert(aegis_agent_loop_create(&config, &loop) == AEGIS_OK);

    assert(aegis_agent_loop_run_turn(loop, "hi") == AEGIS_ERR_MODEL_RATE_LIMIT);
    assert(aegis_agent_loop_run_turn(loop, "hi again") == AEGIS_ERR_CONTEXT_OVERFLOW);
    assert(server.requests == 2);

    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_session_destroy(session);
    aegis_openai_model_destroy(openai);
    pthread_join(thread, NULL);
    close(server_fd);
    puts("openai_http_status_e2e: PASS");
    return 0;
}
