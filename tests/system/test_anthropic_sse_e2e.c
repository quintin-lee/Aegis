/**
 * @file test_anthropic_sse_e2e.c
 * @brief End-to-end: Anthropic SSE stream parsing through the model client.
 *
 * A local mock server returns Anthropic-style SSE frames (event: + data:
 * JSON). The test verifies TEXT deltas, REASONING (thinking) deltas, tool
 * call arguments accumulated across input_json_delta frames, and USAGE
 * capture, plus failure modes (no message_stop, HTTP 400).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/common/error.h"
#include "aegis/model/model.h"
#include "aegis/model/stream.h"
#include "structured_anthropic.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int listen_fd;
    int port;
    int accepts_left;
    int requests;
    /* request capture */
    int saw_xapi_key;
    int saw_version;
    int saw_stream;
    /* response knobs */
    int status_code;
    int close_without_stop;
    int include_thinking;
    int include_tool;
    int with_usage;
    int slow_chunks;
} fixture_t;

typedef struct {
    char     text[256];
    char     reasoning[256];
    size_t   text_len;
    size_t   reasoning_len;
    int      reasoning_deltas;
    int      usage_events;
    uint32_t usage_in;
    uint32_t usage_out;
    uint32_t usage_total;
    int      tool_starts;
    int      tool_deltas;
    int      tool_ends;
    char     tool_name[128];
    char     tool_id[128];
    char     tool_args[256];
    size_t   tool_args_len;
} events_t;

static void append_str(char* dst, size_t cap, size_t* len, const char* s)
{
    if (*len + strlen(s) + 1 < cap) {
        memcpy(dst + *len, s, strlen(s));
        *len += strlen(s);
        dst[*len] = '\0';
    }
}

static aegis_status_t collect_event(const aegis_model_stream_event_t* ev, void* user)
{
    events_t* e = user;
    switch (ev->type) {
    case AEGIS_MODEL_STREAM_TEXT_DELTA:
        append_str(e->text, sizeof(e->text), &e->text_len, (const char*)ev->data);
        break;
    case AEGIS_MODEL_STREAM_REASONING_DELTA:
        append_str(e->reasoning, sizeof(e->reasoning), &e->reasoning_len, (const char*)ev->data);
        ++e->reasoning_deltas;
        break;
    case AEGIS_MODEL_STREAM_TOOL_CALL_START:
        ++e->tool_starts;
        if (ev->tool_name) {
            snprintf(e->tool_name, sizeof(e->tool_name), "%s", ev->tool_name);
        }
        if (ev->call_id) {
            snprintf(e->tool_id, sizeof(e->tool_id), "%s", ev->call_id);
        }
        break;
    case AEGIS_MODEL_STREAM_TOOL_CALL_DELTA:
        ++e->tool_deltas;
        if (ev->tool_name) {
            snprintf(e->tool_name, sizeof(e->tool_name), "%s", ev->tool_name);
        }
        if (ev->call_id) {
            snprintf(e->tool_id, sizeof(e->tool_id), "%s", ev->call_id);
        }
        append_str(e->tool_args, sizeof(e->tool_args), &e->tool_args_len, (const char*)ev->data);
        break;
    case AEGIS_MODEL_STREAM_TOOL_CALL_END:
        ++e->tool_ends;
        break;
    case AEGIS_MODEL_STREAM_USAGE: {
        const aegis_usage_t* u = ev->data;
        ++e->usage_events;
        e->usage_in    = u->input_tokens;
        e->usage_out   = u->output_tokens;
        e->usage_total = u->total_tokens;
        break;
    }
    case AEGIS_MODEL_STREAM_END:
        break;
    default:
        break;
    }
    return AEGIS_OK;
}

static void* fixture_thread(void* arg)
{
    fixture_t* f = arg;
    for (;;) {
        struct sockaddr_in cli;
        socklen_t          clilen = sizeof(cli);
        int                fd     = accept(f->listen_fd, (struct sockaddr*)&cli, &clilen);
        if (fd < 0) {
            break;
        }
        char req[65536];
        int  total = 0;
        while (total < (int)sizeof(req)) {
            int n = read(fd, req + total, sizeof(req) - total);
            if (n <= 0) {
                break;
            }
            total += n;
            if (strstr(req, "\r\n\r\n")) {
                break;
            }
        }
        f->requests++;
        f->saw_xapi_key = req[0] && strstr(req, "x-api-key: test-key") != NULL;
        f->saw_version  = strstr(req, "anthropic-version: 2023-06-01") != NULL;
        f->saw_stream   = strstr(req, "\"stream\":true") != NULL;

        if (f->status_code >= 400) {
            const char* body =
                "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":"
                "\"bad\"}}";
            char hdr[256];
            int  hl = snprintf(
                hdr, sizeof(hdr),
                "HTTP/1.1 %d Bad Request\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                f->status_code, (int)strlen(body));
            (void)write(fd, hdr, (size_t)hl);
            (void)write(fd, body, strlen(body));
            close(fd);
            if (--f->accepts_left == 0) {
                break;
            }
            continue;
        }

        /* Build the Anthropic SSE body. */
        char body[8192];
        int  o = 0;
        char hdr[512];
        memset(hdr, 0, sizeof(hdr));
        if (f->with_usage) {
            o += snprintf(
                body + o, sizeof(body) - o,
                "event: message_start\ndata: "
                "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":11}}}\n\n");
        } else {
            o += snprintf(
                body + o, sizeof(body) - o,
                "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{}}\n\n");
        }
        if (f->include_thinking) {
            o += snprintf(body + o, sizeof(body) - o,
                          "event: content_block_start\ndata: "
                          "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                          "\"type\":\"thinking\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                          "\"thinking_delta\",\"thinking\":\"reasoning A\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                          "\"thinking_delta\",\"thinking\":\"B\"}}\n\n"
                          "event: content_block_stop\ndata: "
                          "{\"type\":\"content_block_stop\",\"index\":0}\n\n");
        }
        int text_index = f->include_thinking ? 1 : 0;
        if (!f->close_without_stop) {
            o += snprintf(body + o, sizeof(body) - o,
                          "event: content_block_start\ndata: "
                          "{\"type\":\"content_block_start\",\"index\":%d,\"content_block\":{"
                          "\"type\":\"text\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":"
                          "\"text_delta\",\"text\":\"Hello\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":"
                          "\"text_delta\",\"text\":\" Claude\"}}\n\n"
                          "event: content_block_stop\ndata: "
                          "{\"type\":\"content_block_stop\",\"index\":%d}\n\n",
                          text_index, text_index, text_index, text_index);
        }
        if (f->include_tool && !f->close_without_stop) {
            int tindex = text_index + 1;
            o += snprintf(body + o, sizeof(body) - o,
                          "event: content_block_start\ndata: "
                          "{\"type\":\"content_block_start\",\"index\":%d,\"content_block\":{"
                          "\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":"
                          "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\"}}\n\n"
                          "event: content_block_delta\ndata: "
                          "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":"
                          "\"input_json_delta\",\"partial_json\":\"\\\"README.md\\\"}\"}}\n\n"
                          "event: content_block_stop\ndata: "
                          "{\"type\":\"content_block_stop\",\"index\":%d}\n\n",
                          tindex, tindex, tindex, tindex);
        }
        if (!f->close_without_stop) {
            if (f->with_usage) {
                o += snprintf(body + o, sizeof(body) - o,
                              "event: message_delta\ndata: "
                              "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}"
                              ",\"usage\":{\"output_tokens\":23}}\n\n");
            }
            o += snprintf(body + o, sizeof(body) - o,
                          "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
        }

        /* Build the HTTP header with Content-Length so curl knows body end. */
        int hl = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n",
                          o);
        if (hl > 0) {
            (void)write(fd, hdr, (size_t)hl);
        }
        if (f->slow_chunks) {
            /* Send in small pieces with a delay to exercise the pending buffer. */
            size_t sent = 0;
            while (sent < (size_t)o) {
                size_t piece = 32;
                if ((size_t)o - sent < piece) {
                    piece = (size_t)o - sent;
                }
                if (write(fd, body + sent, piece) < 0) {
                    break;
                }
                sent += piece;
                struct timespec ts = {0, 5 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
        } else {
            (void)write(fd, body, (size_t)o);
        }
        close(fd);
        if (--f->accepts_left == 0) {
            break;
        }
    }
    close(f->listen_fd);
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
    fixture_t fxt = {
        .listen_fd    = listen_fd,
        .accepts_left = 5,
    };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fixture_thread, &fxt) == 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    aegis_anthropic_model_ctx_t* ctx     = NULL;
    aegis_model_backend_t        backend = {0};
    aegis_status_t               st =
        aegis_anthropic_model_create("test-key", base_url, "test-model", &ctx, &backend);
    assert(st == AEGIS_OK);

    aegis_message_list_t* msgs = NULL;
    aegis_message_t*      user = NULL;
    assert(aegis_message_list_create(&msgs) == AEGIS_OK);
    assert(aegis_message_create(AEGIS_MESSAGE_USER, &user) == AEGIS_OK);
    assert(aegis_message_set_content(user, "hello") == AEGIS_OK);
    assert(aegis_message_list_append(msgs, user) == AEGIS_OK);

    /* Case 1: text + usage. */
    fxt.with_usage            = 1;
    aegis_model_request_t req = {
        .model      = "test-model",
        .messages   = msgs,
        .stream     = true,
        .max_tokens = 100,
    };
    events_t evs = {0};
    /* Use the backend's stream directly (no session needed). */
    st = backend.stream(backend.user, &req, NULL, collect_event, &evs);
    assert(st == AEGIS_OK);
    assert(fxt.saw_xapi_key && fxt.saw_version && fxt.saw_stream);
    assert(strcmp(evs.text, "Hello Claude") == 0);
    assert(evs.usage_events == 1);
    assert(evs.usage_in == 11 && evs.usage_out == 23 && evs.usage_total == 34);

    /* Case 2: thinking deltas -> REASONING. */
    memset(&evs, 0, sizeof(evs));
    fxt.include_thinking = 1;
    fxt.with_usage       = 0;
    st                   = backend.stream(backend.user, &req, NULL, collect_event, &evs);
    assert(st == AEGIS_OK);
    assert(evs.reasoning_deltas == 2);
    assert(strcmp(evs.reasoning, "reasoning AB") == 0);

    /* Case 3: tool call args accumulated across input_json_delta frames. */
    memset(&evs, 0, sizeof(evs));
    fxt.include_thinking = 0;
    fxt.include_tool     = 1;
    fxt.with_usage       = 1;
    st                   = backend.stream(backend.user, &req, NULL, collect_event, &evs);
    assert(st == AEGIS_OK);
    assert(evs.tool_ends == 1);
    assert(evs.tool_deltas == 1);
    assert(strcmp(evs.tool_name, "read") == 0);
    assert(strcmp(evs.tool_id, "toolu_1") == 0);
    assert(strcmp(evs.tool_args, "{\"path\":\"README.md\"}") == 0);

    /* Case 4: stream closes without message_stop -> PROVIDER. */
    memset(&evs, 0, sizeof(evs));
    fxt.close_without_stop = 1;
    st                     = backend.stream(backend.user, &req, NULL, collect_event, &evs);
    assert(st == AEGIS_ERR_PROVIDER);

    /* Case 5: HTTP 400 -> PROVIDER. */
    fxt.close_without_stop = 0;
    fxt.status_code        = 400;
    memset(&evs, 0, sizeof(evs));
    st = backend.stream(backend.user, &req, NULL, collect_event, &evs);
    assert(st == AEGIS_ERR_PROVIDER);

    aegis_message_list_destroy(msgs);
    aegis_anthropic_model_destroy(ctx);
    fxt.status_code = 0;
    pthread_join(thread, NULL);
    puts("PASS");
    return 0;
}
