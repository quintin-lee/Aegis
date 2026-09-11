/**
 * @file cli_repl.h
 * @brief Shared internals of the interactive REPL (input/gate/render/loop).
 *
 * Private to apps/aegis interactive mode. Not part of libaegis public API.
 * Split from cli_interactive.c by responsibility:
 *   cli_input.c  — stdin reader thread, line queue, raw terminal mode
 *   cli_gate.c   — approval gate handshake, turn watcher, approval callback
 *   cli_render.c — banner, streaming renderer, tool listing
 *   cli_interactive.c — REPL loop, slash commands, turn dispatch
 */
#ifndef AEGIS_CLI_REPL_H
#define AEGIS_CLI_REPL_H

#include "aegis/agent/loop.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct aegis_coding_agent aegis_coding_agent_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Streaming renderer state (lives in the REPL frame) ─────────────────── */

typedef struct cli_stream_ctx {
    bool            enabled;
    bool            text_emitted;
    bool            line_open;
    bool            reasoning_open;
    bool            tool_running;
    struct timespec tool_start;
    bool            approvals;
    char            allowed_tools[16][64];
    size_t          allowed_count;
} cli_stream_ctx_t;

void cli_stream_prelude(cli_stream_ctx_t* cx);
bool json_mode_env(void);
void print_banner(const char* model);
void cli_event_cb(const aegis_agent_event_t* ev, void* user);
aegis_status_t cli_print_tool_def(const aegis_tool_def_t* def, void* user);

/* ── stdin line queue (reader thread → consumers) ────────────────────────── */

typedef struct line_cell {
    char*             text;
    struct line_cell* next;
} line_cell_t;

typedef struct line_queue {
    line_cell_t*    head;
    line_cell_t*    tail;
    bool            closed;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} line_queue_t;

void   lq_init(line_queue_t* q);
void   lq_push(line_queue_t* q, char* text);
char*  lq_pop(line_queue_t* q);
void   lq_close(line_queue_t* q);
void   raw_enable(void);
void   raw_disable(void);
void*  reader_main(void* arg);

extern line_queue_t        g_lines;
extern volatile sig_atomic_t g_reader_shutdown;

/* ── Approval gate + turn watcher ────────────────────────────────────────── */

typedef struct watcher_ctx {
    aegis_coding_agent_t* agent;
    char**                pending;
    size_t                n;
    size_t                cap;
    bool                  interrupted;
} watcher_ctx_t;

char*  gate_take(void);
void*  watcher_main(void* arg);
aegis_tool_approval_t cli_approval_cb(const char* tool_name, const char* args_json,
                                      void* user);

extern watcher_ctx_t* g_gate_watcher;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CLI_REPL_H */
