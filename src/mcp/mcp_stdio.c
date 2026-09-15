/**
 * @file mcp_stdio.c
 * @brief MCP stdio transport: spawn an MCP server subprocess and speak
 *        newline-delimited JSON-RPC 2.0 over its stdin/stdout.
 *
 * Subprocess lifecycle mirrors src/coding/git_tools.c (fork/execvp with
 * setpgid, SIGTERM/SIGKILL on timeout, waitpid). The difference is two
 * pipes (child stdin + child stdout) and newline-delimited framing rather
 * than bulk output capture.
 */
#define _POSIX_C_SOURCE 200809L
#include "mcp_stdio.h"
#include "aegis/common/cancellation/cancellation.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MCP_STDIO_TIMEOUT_MS 60000L
#define MCP_MAX_LINE         (1024u * 1024u)

struct aegis_mcp_stdio {
    pid_t    pid;
    int      to_child;   /* write end: parent -> child stdin */
    int      from_child; /* read end:  child stdout -> parent */
    uint64_t next_id;    /* ids not yet sent */
};

/* Send one JSON-RPC line to the child's stdin. Returns AEGIS_OK on success. */
static aegis_status_t send_line(aegis_mcp_stdio_t* s, const char* line, size_t len)
{
    /* Append a newline and write the whole thing atomically. */
    char* buf = malloc(len + 2);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    memcpy(buf, line, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';
    size_t off   = 0;
    while (off < len + 1) {
        ssize_t w = write(s->to_child, buf + off, len + 1 - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return AEGIS_ERR_PROVIDER;
        }
        off += (size_t)w;
    }
    free(buf);
    return AEGIS_OK;
}

/* Read one newline-delimited JSON line from the child's stdout into a
 * NUL-terminated buffer. Grows as needed (cap starts 4096). Returns 1 on a
 * full line, 0 on EOF, -1 on read error / line too long. */
static int read_line(aegis_mcp_stdio_t* s, char** line, size_t* len, size_t* cap)
{
    if (*len + 1 + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 4096;
        while (ncap < *len + 2) {
            ncap *= 2;
        }
        if (ncap > MCP_MAX_LINE) {
            return -1;
        }
        char* p = realloc(*line, ncap);
        if (!p) {
            return -1;
        }
        *line = p;
        *cap  = ncap;
    }
    char c = 0;
    while (c != '\n') {
        ssize_t r = read(s->from_child, &c, 1);
        if (r == 0) {
            /* EOF before newline: flush what we have (if anything). */
            (*line)[*len] = '\0';
            return *len ? 1 : 0;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        (*line)[(*len)++] = c;
    }
    (*line)[*len] = '\0';
    /* Drop the trailing newline so the buffer is clean JSON. */
    --(*len);
    (*line)[*len] = '\0';
    return 1;
}

/* Perform the initialize handshake: send initialize, then the initialized
 * notification. The response DOM from initialize is parsed and discarded. */
static aegis_status_t do_handshake(aegis_mcp_stdio_t* s)
{
    /* {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{}}}
     */
    static const char* init_req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{}}}";
    aegis_status_t st = send_line(s, init_req, strlen(init_req));
    if (st != AEGIS_OK) {
        return st;
    }
    /* Read lines until we get the response for id 1, or EOF. */
    char*  line = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        int rc = read_line(s, &line, &len, &cap);
        if (rc < 0) {
            free(line);
            return AEGIS_ERR_PROVIDER;
        }
        if (rc == 0) {
            /* EOF during handshake. */
            free(line);
            return AEGIS_ERR_PROVIDER;
        }
        aegis_json_value_t* resp = NULL;
        if (aegis_json_parse(line, &resp) != AEGIS_OK) {
            continue; /* not valid JSON; skip */
        }
        const aegis_json_value_t* rid = aegis_json_object_get(resp, "id");
        if (rid && aegis_json_int(rid) == 1) {
            aegis_json_value_destroy(resp);
            free(line);
            break;
        }
        aegis_json_value_destroy(resp);
    }
    /* Send the "initialized" notification (no id, no response expected). */
    static const char* init_notif =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    st = send_line(s, init_notif, strlen(init_notif));
    return st;
}

aegis_status_t aegis_mcp_stdio_spawn(const char* cmd, const char* const* argv,
                                     aegis_mcp_stdio_t** out)
{
    if (!out || !cmd) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        return AEGIS_ERR_INTERNAL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return AEGIS_ERR_INTERNAL;
    }
    if (pid == 0) {
        /* child */
        setpgid(0, 0);
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        /* Build argv array: [cmd, argv[0], argv[1], ..., NULL]. */
        char** child_argv = NULL;
        int    argc       = 1;
        if (argv) {
            for (int i = 0; argv[i]; ++i) {
                ++argc;
            }
        }
        child_argv = calloc((size_t)argc + 1, sizeof(char*));
        if (child_argv) {
            child_argv[0] = (char*)cmd;
            int i         = 1;
            if (argv) {
                for (int j = 0; argv[j]; ++j) {
                    child_argv[i++] = (char*)argv[j];
                }
            }
            child_argv[i] = NULL;
            execvp(cmd, child_argv);
        }
        _exit(127);
    }

    /* parent */
    setpgid(pid, pid);
    close(in_pipe[0]);
    close(out_pipe[1]);

    aegis_mcp_stdio_t* s = calloc(1, sizeof(*s));
    if (!s) {
        kill(-pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(in_pipe[1]);
        close(out_pipe[0]);
        return AEGIS_ERR_NOMEM;
    }
    s->pid        = pid;
    s->to_child   = in_pipe[1];
    s->from_child = out_pipe[0];
    s->next_id    = 2; /* id 1 reserved for initialize */

    aegis_status_t st = do_handshake(s);
    if (st != AEGIS_OK) {
        aegis_mcp_stdio_destroy(s);
        return st;
    }
    *out = s;
    return AEGIS_OK;
}

void aegis_mcp_stdio_destroy(aegis_mcp_stdio_t* s)
{
    if (!s) {
        return;
    }
    if (s->to_child >= 0) {
        close(s->to_child);
    }
    if (s->from_child >= 0) {
        close(s->from_child);
    }
    if (s->pid > 0) {
        kill(-s->pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 5; ++i) {
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
            if (waitpid(s->pid, &status, WNOHANG) == s->pid) {
                break;
            }
        }
        if (waitpid(s->pid, &status, WNOHANG) != s->pid) {
            kill(-s->pid, SIGKILL);
            waitpid(s->pid, &status, 0);
        }
    }
    free(s);
}

aegis_status_t aegis_mcp_stdio_request(aegis_mcp_stdio_t* s, uint64_t id, const char* method,
                                       const aegis_json_value_t* params,
                                       aegis_json_value_t**      out_result)
{
    if (!s || !method || !out_result) {
        return AEGIS_ERR_INVALID;
    }
    *out_result = NULL;

    /* Build the params JSON (or omit if NULL). */
    char* params_json = NULL;
    if (params) {
        aegis_status_t pst = aegis_json_serialize(params, &params_json);
        if (pst != AEGIS_OK) {
            return pst;
        }
    }

    /* Build the full request line. */
    size_t cap = 1024;
    char*  req = malloc(cap);
    if (!req) {
        free(params_json);
        return AEGIS_ERR_NOMEM;
    }
    int wn = 0;
    if (params_json) {
        wn = snprintf(req, cap, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\",\"params\":%s}",
                      (unsigned long long)id, method, params_json);
    } else {
        wn = snprintf(req, cap, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\"}",
                      (unsigned long long)id, method);
    }
    free(params_json);
    if (wn < 0 || (size_t)wn >= cap) {
        free(req);
        return AEGIS_ERR_NOMEM;
    }

    aegis_status_t st = send_line(s, req, (size_t)wn);
    free(req);
    if (st != AEGIS_OK) {
        return st;
    }

    /* Read lines until we find the response matching this id. */
    char*  line = NULL;
    size_t len = 0, cap2 = 0;
    for (;;) {
        int rc = read_line(s, &line, &len, &cap2);
        if (rc < 0) {
            free(line);
            return AEGIS_ERR_PROVIDER;
        }
        if (rc == 0) {
            /* EOF: child exited. */
            free(line);
            return AEGIS_ERR_PROVIDER;
        }
        aegis_json_value_t* resp = NULL;
        if (aegis_json_parse(line, &resp) != AEGIS_OK) {
            continue;
        }
        const aegis_json_value_t* rid = aegis_json_object_get(resp, "id");
        if (rid && aegis_json_int(rid) == (int64_t)id) {
            /* Matched. */
            const aegis_json_value_t* err = aegis_json_object_get(resp, "error");
            if (err) {
                aegis_json_value_destroy(resp);
                free(line);
                return AEGIS_ERR_PROVIDER;
            }
            /* The "result" value is a child of resp. Copy it out before
             * destroying resp, so the caller receives an independent owned DOM. */
            const aegis_json_value_t* result_node = aegis_json_object_get(resp, "result");
            aegis_json_value_t*       detached    = NULL;
            if (result_node) {
                char* rjson = NULL;
                if (aegis_json_serialize(result_node, &rjson) == AEGIS_OK) {
                    if (aegis_json_parse(rjson, &detached) != AEGIS_OK) {
                        detached = NULL;
                    }
                    free(rjson);
                }
            }
            aegis_json_value_destroy(resp);
            free(line);
            if (id >= s->next_id) {
                s->next_id = id + 1;
            }
            *out_result = detached;
            return AEGIS_OK;
        }
        aegis_json_value_destroy(resp);
    }
}
