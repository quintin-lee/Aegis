/**
 * @file test_anthropic_http_status_e2e.c
 * @brief End-to-end: Anthropic provider surfaces 429 as MODEL_RATE_LIMIT and
 * 413 as CONTEXT_OVERFLOW through the agent loop.
 *
 * A local mock server answers two consecutive POST /v1/messages requests
 * with 429 then 413. The agent loop run must surface the corresponding
 * aegis_status_t codes.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/common/error.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
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
    int status_seq[2];
} server_t;

static void* server_thread(void* arg)
{
    server_t* s = arg;
    char      buf[16384];
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
        int         status = s->status_seq[i];
        const char* body   = status == 429 ? "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_"
                                             "error\",\"message\":\"429\"}}"
                                           : "{\"type\":\"error\",\"error\":{\"type\":\"invalid_"
                                             "request_error\",\"message\":\"413\"}}";
        char        header[256];
        int         hl = snprintf(header, sizeof(header),
                                  "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                                  "Content-Length: %d\r\nConnection: close\r\n\r\n",
                                  status, status == 429 ? "Too Many Requests" : "Payload Too Large",
                                  (int)strlen(body));
        (void)write(fd, header, (size_t)hl);
        (void)write(fd, body, strlen(body));
        close(fd);
    }
    return NULL;
}

static int make_server(server_t* s)
{
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = 0;
    if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        return -1;
    }
    if (listen(s->listen_fd, 8) < 0) {
        return -1;
    }
    struct sockaddr_in bound;
    socklen_t          blen = sizeof(bound);
    getsockname(s->listen_fd, (struct sockaddr*)&bound, &blen);
    s->port = ntohs(bound.sin_port);
    return 0;
}

int main(void)
{
    server_t srv      = {0};
    srv.status_seq[0] = 429;
    srv.status_seq[1] = 413;
    assert(make_server(&srv) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, &srv) == 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", srv.port);

    aegis_anthropic_model_ctx_t* ctx     = NULL;
    aegis_model_backend_t        backend = {0};
    aegis_status_t               st =
        aegis_anthropic_model_create("test-key", base_url, "test-model", &ctx, &backend);
    assert(st == AEGIS_OK);

    aegis_session_t* session = NULL;
    assert(aegis_session_create(".", &session) == AEGIS_OK);

    aegis_model_client_t* model = NULL;
    st = aegis_model_client_create_with_backend("test-model", &backend, &model);
    assert(st == AEGIS_OK);

    aegis_agent_loop_config_t cfg = {
        .session       = session,
        .model         = model,
        .tools         = NULL,
        .system_prompt = "fixture",
    };
    aegis_agent_loop_t* loop = NULL;
    assert(aegis_agent_loop_create(&cfg, &loop) == AEGIS_OK);

    st = aegis_agent_loop_run_turn(loop, "request 429");
    assert(st == AEGIS_ERR_MODEL_RATE_LIMIT);
    st = aegis_agent_loop_run_turn(loop, "request 413");
    assert(st == AEGIS_ERR_CONTEXT_OVERFLOW);

    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_session_destroy(session);
    aegis_anthropic_model_destroy(ctx);
    pthread_join(thread, NULL);
    close(srv.listen_fd);
    assert(srv.requests == 2);
    puts("PASS");
    return 0;
}
