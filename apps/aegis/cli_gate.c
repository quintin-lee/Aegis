#define _POSIX_C_SOURCE 200809L
#include "cli_repl.h"
#include "aegis/coding/coding_agent.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* While an approval prompt waits, the next line is handed to the gate
 * through a small handshake slot instead of the pending FIFO. */
pthread_mutex_t g_gate_mu      = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_gate_cv      = PTHREAD_COND_INITIALIZER;
char*           g_gate_line    = NULL; /**< next answer for the gate */
bool            g_gate_waiting = false;
bool            g_gate_eof     = false;

/** Hand a line to the waiting approval gate (takes ownership of @p line). */
static void gate_put(char* line)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate_line = line;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

/** Watcher during a turn: empty line -> interrupt once; lines -> pending FIFO. */
watcher_ctx_t* g_gate_watcher = NULL; /**< pending-FIFO owner */

/** Blocking take for the approval gate; NULL = EOF (ownership transferred). */
char* gate_take(void)
{
    /* Fast path: the watcher may have handed a line over already. */
    pthread_mutex_lock(&g_gate_mu);
    if (g_gate_line) {
        char* ready = g_gate_line;
        g_gate_line = NULL;
        pthread_mutex_unlock(&g_gate_mu);
        return ready;
    }
    g_gate_waiting = true;
    pthread_mutex_unlock(&g_gate_mu);

    /* Lines typed before the gate opened may sit in the watcher's pending
     * FIFO: drain the oldest one first. */
    watcher_ctx_t* w = g_gate_watcher;
    if (w && w->n > 0) {
        char* queued = w->pending[0];
        memmove(w->pending, w->pending + 1, (w->n - 1) * sizeof(char*));
        w->n--;
        pthread_mutex_lock(&g_gate_mu);
        g_gate_waiting = false;
        pthread_mutex_unlock(&g_gate_mu);
        return queued;
    }

    /* Nothing queued: pull directly from the reader queue. */
    char* line = lq_pop(&g_lines);
    if (!line) {
        pthread_mutex_lock(&g_gate_mu);
        g_gate_waiting = false;
        pthread_mutex_unlock(&g_gate_mu);
        return NULL; /* EOF => deny */
    }
    pthread_mutex_lock(&g_gate_mu);
    g_gate_waiting = false;
    pthread_mutex_unlock(&g_gate_mu);
    return line;
}

void* watcher_main(void* arg)
{
    watcher_ctx_t* w = arg;
    while (1) {
        char* line = lq_pop(&g_lines);
        if (!line) {
            pthread_mutex_lock(&g_gate_mu);
            g_gate_eof = true;
            pthread_cond_broadcast(&g_gate_cv);
            pthread_mutex_unlock(&g_gate_mu);
            break; /* EOF: stop watching; leftover lines stay queued */
        }
        {
            /* Hand the line to the approval gate if it is waiting. */
            pthread_mutex_lock(&g_gate_mu);
            bool waiting = g_gate_waiting;
            pthread_mutex_unlock(&g_gate_mu);
            if (waiting) {
                gate_put(line);
                continue;
            }
        }
        if (line[0] == '\0' || strcmp(line, "/stop") == 0) {
            if (!w->interrupted) {
                w->interrupted = true;
                aegis_coding_agent_interrupt(w->agent);
            }
            free(line);
            continue;
        }
        if (w->n == w->cap) {
            size_t cap = w->cap ? w->cap * 2 : 8;
            char** p   = realloc(w->pending, cap * sizeof(char*));
            if (!p) {
                free(line);
                continue;
            }
            w->pending = p;
            w->cap     = cap;
        }
        w->pending[w->n++] = line;
    }
    return NULL;
}

/* Approval gate: interactive y/n/a unless disabled or tool allow-listed. */
aegis_tool_approval_t cli_approval_cb(const char* tool_name, const char* args_json,
                                      void* user)
{
    cli_stream_ctx_t* cx = (cli_stream_ctx_t*)user;
    if (!cx || !cx->approvals || !tool_name) {
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    for (size_t i = 0; i < cx->allowed_count; i++) {
        if (strcmp(cx->allowed_tools[i], tool_name) == 0) {
            return AEGIS_TOOL_APPROVAL_ALLOW;
        }
    }
    cli_stream_prelude(cx);
    if (cx->reasoning_open) {
        fputs("\033[0m\n", stdout);
        cx->reasoning_open = false;
        cx->line_open      = false;
    }
    printf("approve %s %s? [y/n/a] ", tool_name, args_json ? args_json : "");
    fflush(stdout);
    /* Answers come through the gate handshake: the watcher thread routes
     * the next typed line here (the reader thread owns stdin). */
    char* answer_line = gate_take();
    cx->line_open     = false;
    if (!answer_line) {
        return AEGIS_TOOL_APPROVAL_DENY; /* EOF => deny */
    }
    char verdict = answer_line[0];
    free(answer_line);
    if (verdict == 'a') {
        if (cx->allowed_count < 16) {
            snprintf(cx->allowed_tools[cx->allowed_count++], sizeof(cx->allowed_tools[0]), "%s",
                     tool_name);
        }
        return AEGIS_TOOL_APPROVAL_ALLOW; /* list full degrades to y */
    }
    if (verdict == 'y') {
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    return AEGIS_TOOL_APPROVAL_DENY;
}
