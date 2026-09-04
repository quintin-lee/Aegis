#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "aegis/coding/coding_agent.h"
#include "aegis/session/session.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void print_banner(const char* model)
{
    printf("Aegis Coding Agent\n");
    printf("project: %s\n", ".");
    printf("model: %s\n", model ? model : "mock");
    printf("type /help for commands\n\n");
}

/* ── Live streaming output ────────────────────────────────────────── */

typedef struct cli_stream_ctx {
    bool        enabled;        /**< /stream on|off                              */
    bool        text_emitted;   /**< streamed text already shown for this turn   */
    bool        line_open;      /**< current line has unterminated content       */
    bool        reasoning_open; /**< dim-italic reasoning block is being printed */
    bool        tool_running;   /**< TOOL_START seen, awaiting TOOL_END          */
    struct timespec tool_start; /**< wall clock at TOOL_START                    */
    bool        approvals;      /**< /approvals on|off                           */
    char        allowed_tools[16][64]; /**< always-allow list (session-lifetime) */
    size_t      allowed_count;
} cli_stream_ctx_t;

static void cli_stream_prelude(cli_stream_ctx_t* cx)
{
    if (cx->line_open) {
        putchar('\n');
        cx->line_open = false;
    }
}

/* AEGIS_JSON=1 forces machine-readable output: no live streaming callback. */
static bool json_mode_env(void)
{
    const char* env = getenv("AEGIS_JSON");
    return env && strcmp(env, "1") == 0;
}

/* ── stdin reader thread + line queue ────────────────────────────────────
 * A background thread reads stdin into a FIFO so lines typed during a
 * running turn are not lost: an empty line interrupts the turn, a
 * non-empty line queues as the next input. */
typedef struct line_cell {
    char*             text;  /**< Heap copy; NULL = EOF sentinel. */
    struct line_cell* next;
} line_cell_t;

typedef struct line_queue {
    line_cell_t*    head;
    line_cell_t*    tail;
    bool            closed;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} line_queue_t;

static void lq_init(line_queue_t* q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static void lq_push(line_queue_t* q, char* text)
{
    line_cell_t* c = malloc(sizeof(*c));
    if (!c) {
        free(text);
        return;
    }
    *c = (line_cell_t){.text = text, .next = NULL};
    pthread_mutex_lock(&q->mu);
    if (q->tail) {
        q->tail->next = c;
    } else {
        q->head = c;
    }
    q->tail = c;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/** Blocking pop; returns NULL on EOF. */
static char* lq_pop(line_queue_t* q)
{
    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed) {
        pthread_cond_wait(&q->cv, &q->mu);
    }
    line_cell_t* c    = q->head;
    char*        text = NULL;
    if (c) {
        q->head = c->next;
        if (!q->head) {
            q->tail = NULL;
        }
        text = c->text;
        free(c);
    }
    pthread_mutex_unlock(&q->mu);
    return text;
}

static void lq_close(line_queue_t* q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static line_queue_t g_lines;

static void* reader_main(void* arg)
{
    (void)arg;
    char buf[4096];
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        lq_push(&g_lines, strdup(buf));
    }
    lq_push(&g_lines, NULL); /* EOF sentinel */
    return NULL;
}

/* While an approval prompt waits, the next line is handed to the gate
 * through a small handshake slot instead of the pending FIFO. */
static pthread_mutex_t g_gate_mu     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv     = PTHREAD_COND_INITIALIZER;
static char*           g_gate_line   = NULL; /**< next answer for the gate */
static bool            g_gate_waiting = false;
static bool            g_gate_eof     = false;

/** Hand a line to the waiting approval gate (takes ownership of @p line). */
static void gate_put(char* line)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate_line = line;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

/** Watcher during a turn: empty line -> interrupt once; lines -> pending FIFO. */
typedef struct watcher_ctx {
    aegis_coding_agent_t* agent;
    char**                pending;
    size_t                n;
    size_t                cap;
    bool                  interrupted;
} watcher_ctx_t;

static watcher_ctx_t* g_gate_watcher = NULL; /**< pending-FIFO owner */

/** Blocking take for the approval gate; NULL = EOF (ownership transferred). */
static char* gate_take(void)
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

static void* watcher_main(void* arg)
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
        if (line[0] == '\0') {
            if (!w->interrupted) {
                w->interrupted = true;
                aegis_coding_agent_interrupt(w->agent);
            }
            free(line);
            continue;
        }
        if (w->n == w->cap) {
            size_t     cap  = w->cap ? w->cap * 2 : 8;
            char**     p    = realloc(w->pending, cap * sizeof(char*));
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

/* /tools visitor: print "name — description" plus the parameter list. */
static const char* cli_val_type_str(aegis_tool_value_type_t t)
{
    switch (t) {
    case AEGIS_TOOL_VAL_STRING: return "string";
    case AEGIS_TOOL_VAL_INT: return "int";
    case AEGIS_TOOL_VAL_FLOAT: return "float";
    case AEGIS_TOOL_VAL_BOOL: return "bool";
    default: return "bytes";
    }
}

static aegis_status_t cli_print_tool_def(const aegis_tool_def_t* def, void* user)
{
    (void)user;
    printf("  %s — %s\n", def->name, def->description ? def->description : "(no description)");
    for (size_t i = 0; def->schema.params && i < def->schema.param_count; i++) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (p->description) {
            printf("    %s%s: %s (%s)\n", p->name, p->required ? "" : "?", cli_val_type_str(p->type),
                   p->description);
        } else {
            printf("    %s%s: %s\n", p->name, p->required ? "" : "?", cli_val_type_str(p->type));
        }
    }
    return AEGIS_OK;
}

/* Approval gate: interactive y/n/a unless disabled or tool allow-listed. */
static aegis_tool_approval_t cli_approval_cb(const char* tool_name, const char* args_json,
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
    cx->line_open = false;
    if (!answer_line) {
        return AEGIS_TOOL_APPROVAL_DENY; /* EOF => deny */
    }
    char verdict = answer_line[0];
    free(answer_line);
    if (verdict == 'a') {
        if (cx->allowed_count < 16) {
            snprintf(cx->allowed_tools[cx->allowed_count++],
                     sizeof(cx->allowed_tools[0]), "%s", tool_name);
        }
        return AEGIS_TOOL_APPROVAL_ALLOW; /* list full degrades to y */
    }
    if (verdict == 'y') {
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    return AEGIS_TOOL_APPROVAL_DENY;
}

static void cli_event_cb(const aegis_agent_event_t* ev, void* user)
{
    cli_stream_ctx_t* cx = (cli_stream_ctx_t*)user;
    if (!cx->enabled) {
        return;
    }
    switch (ev->type) {
    case AEGIS_AGENT_EVENT_REASONING_DELTA:
        if (ev->data && ev->len) {
            cli_stream_prelude(cx);
            if (!cx->reasoning_open) {
                fputs("\033[2m\033[3m", stdout);
                cx->reasoning_open = true;
            }
            fwrite(ev->data, 1, ev->len, stdout);
            fflush(stdout);
        }
        break;
    case AEGIS_AGENT_EVENT_TEXT_DELTA:
        if (cx->reasoning_open) {
            fputs("\033[0m\n", stdout);
            cx->reasoning_open = false;
            cx->line_open      = false;
        }
        if (ev->data && ev->len) {
            fwrite(ev->data, 1, ev->len, stdout);
            cx->text_emitted = true;
            cx->line_open    = true;
            fflush(stdout);
        }
        break;
    case AEGIS_AGENT_EVENT_TOOL_START:
        if (cx->reasoning_open) {
            fputs("\033[0m\n", stdout);
            cx->reasoning_open = false;
            cx->line_open      = false;
        }
        cli_stream_prelude(cx);
        printf("● %s", ev->tool_name ? ev->tool_name : "?");
        cx->line_open    = true;
        cx->tool_running = true;
        clock_gettime(CLOCK_MONOTONIC, &cx->tool_start);
        fflush(stdout);
        break;
    case AEGIS_AGENT_EVENT_TOOL_END: {
        /* Elapsed wall time since TOOL_START; <1s as ms, otherwise s. */
        char timing[24] = "";
        if (cx->tool_running) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - cx->tool_start.tv_sec) * 1000LL +
                           (now.tv_nsec - cx->tool_start.tv_nsec) / 1000000LL;
            if (ms < 0) {
                ms = 0;
            }
            if (ms < 1000) {
                snprintf(timing, sizeof(timing), " (%lldms)", ms);
            } else {
                snprintf(timing, sizeof(timing), " (%.1fs)", (double)ms / 1000.0);
            }
            cx->tool_running = false;
        }
        if (ev->status == AEGIS_OK) {
            char preview[64] = {0};
            if (ev->data && ev->len) {
                const char* s    = (const char*)ev->data;
                size_t      take = ev->len < sizeof(preview) - 1 ? ev->len : sizeof(preview) - 1;
                memcpy(preview, s, take);
                for (size_t i = 0; i < take; i++) {
                    if (preview[i] == '\n') {
                        preview[i] = ' ';
                    }
                }
            }
            printf("  ✓ %s%s\n", preview[0] ? preview : "ok", timing);
        } else {
            printf("  ✗ %s%s\n", aegis_status_str(ev->status), timing);
        }
        cx->line_open = false;
        fflush(stdout);
        break;
    }
    default:
        break;
    }
}

int cmd_interactive(const char* project_root, const char* model, const char* resume_path)
{
    aegis_coding_agent_config_t cfg = {0};
    cfg.project_root                = project_root ? project_root : ".";
    cfg.model                       = model ? model : "mock";
    cfg.provider                    = getenv("AEGIS_PROVIDER");
    cfg.api_key                     = getenv("OPENAI_API_KEY");
    cfg.base_url                    = getenv("AEGIS_OPENAI_BASE_URL");
    aegis_coding_agent_t* agent     = NULL;
    aegis_status_t        st        = aegis_coding_agent_create(&cfg, &agent);
    if (st != AEGIS_OK) {
        fprintf(stderr, "failed to create coding agent: %s\n", aegis_status_str(st));
        return 1;
    }
    if (resume_path) {
        aegis_session_t* loaded = NULL;
        if (aegis_session_load(resume_path, &loaded) == AEGIS_OK) {
            st = aegis_coding_agent_replace_session(agent, loaded);
            if (st == AEGIS_OK) {
                printf("resumed session %s with %zu messages\n", aegis_session_id(loaded),
                       aegis_session_message_count(loaded));
                loaded = NULL;
            } else {
                fprintf(stderr, "failed to resume session: %s\n", aegis_status_str(st));
            }
            aegis_session_destroy(loaded);
        }
    }
    print_banner(aegis_coding_agent_model_name(agent));
    static cli_stream_ctx_t stream_ctx = {.enabled        = true,
                                          .text_emitted   = false,
                                          .line_open      = false,
                                          .approvals      = false,
                                          .allowed_count  = 0};
    aegis_coding_agent_set_tool_approval(agent, cli_approval_cb, &stream_ctx);
    if (!json_mode_env()) {
        aegis_coding_agent_set_event_callback(agent, cli_event_cb, &stream_ctx);
    }
    int         json_mode = json_mode_env();
    lq_init(&g_lines);
    pthread_t   reader;
    bool        reader_up = pthread_create(&reader, NULL, reader_main, NULL) == 0;
    while (1) {
        printf("> ");
        fflush(stdout);
        char* line = reader_up ? lq_pop(&g_lines) : NULL;
        if (!line) {
            break; /* EOF */
        }
        if (line[0] == '\0') {
            free(line);
            continue;
        }
        if (strcmp(line, "/help") == 0 || strcmp(line, "/h") == 0) {
            printf("/help /model /tools /session /sessions /resume /fork /tree /compact /json /stream /approvals /clear /quit\n");
            continue;
        }
        if (strcmp(line, "/tools") == 0) {
            aegis_tool_registry_t* tools = NULL;
            aegis_status_t        stt   = aegis_coding_agent_tools(agent, &tools);
            if (stt != AEGIS_OK) {
                printf("error: %s\n", aegis_status_str(stt));
                continue;
            }
            size_t count = aegis_tool_registry_count(tools);
            if (count == 0) {
                printf("no tools registered\n");
                continue;
            }
            printf("registered tools (%zu):\n", count);
            aegis_tool_registry_visit(tools, cli_print_tool_def, NULL);
            continue;
        }
        if (strcmp(line, "/model") == 0 || strncmp(line, "/model ", 7) == 0) {
            const char* arg = line[6] == ' ' ? line + 7 : NULL;
            if (!arg || arg[0] == '\0') {
                printf("model: %s\n", aegis_coding_agent_model_name(agent));
            } else {
                aegis_status_t st3 = aegis_coding_agent_set_model(agent, arg);
                if (st3 == AEGIS_OK) {
                    printf("switched model to %s\n", aegis_coding_agent_model_name(agent));
                } else {
                    printf("error: %s\n", aegis_status_str(st3));
                }
            }
            continue;
        }
        if (strcmp(line, "/session") == 0 || strcmp(line, "/tree") == 0) {
            aegis_session_t* sess = aegis_coding_agent_session(agent);
            printf("session %s branch %s parent %s messages %zu\n", aegis_session_id(sess),
                   aegis_session_branch_id(sess),
                   aegis_session_parent_id(sess) ? aegis_session_parent_id(sess) : "-",
                   aegis_session_message_count(sess));
            continue;
        }
        if (strcmp(line, "/sessions") == 0) {
            DIR* d = opendir(".aegis");
            if (!d) {
                printf("(no saved sessions)\n");
                continue;
            }
            struct dirent* ent;
            bool           any = false;
            while ((ent = readdir(d)) != NULL) {
                const char* n = ent->d_name;
                size_t      l = strlen(n);
                if (strncmp(n, "session-", 8) != 0 || l < 8 + 4 || strcmp(n + l - 6, ".jsonl") != 0) {
                    continue;
                }
                char        pbuf[1024];
                snprintf(pbuf, sizeof(pbuf), ".aegis/%s", n);
                struct stat st;
                if (stat(pbuf, &st) != 0) {
                    continue;
                }
                char tsbuf[32];
                struct tm tm_v;
                localtime_r(&st.st_mtime, &tm_v);
                strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm_v);
                printf("%s  %s\n", n, tsbuf);
                any = true;
            }
            closedir(d);
            if (!any) {
                printf("(no saved sessions)\n");
            }
            continue;
        }
        if (strncmp(line, "/resume", 7) == 0 && (line[7] == '\0' || line[7] == ' ')) {
            const char* arg = line[7] == ' ' ? line + 8 : NULL;
            if (!arg || arg[0] == '\0') {
                printf("usage: /resume <session-file>\n");
                continue;
            }
            aegis_session_t* loaded = NULL;
            aegis_status_t   st3    = aegis_session_load(arg, &loaded);
            if (st3 != AEGIS_OK) {
                printf("resume failed: %s\n", aegis_status_str(st3));
                continue;
            }
            st3 = aegis_coding_agent_replace_session(agent, loaded);
            if (st3 != AEGIS_OK) {
                aegis_session_destroy(loaded);
                printf("resume failed: %s\n", aegis_status_str(st3));
                continue;
            }
            printf("resumed %s (%zu messages)\n", aegis_session_id(loaded),
                   aegis_session_message_count(loaded));
            continue;
        }
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0 || strcmp(line, "/q") == 0) {
            free(line);
            break;
        }
        if (strncmp(line, "/clear", 6) == 0) {
            printf("\033[2J\033[H");
            continue;
        }
        if (strcmp(line, "/fork") == 0) {
            aegis_session_t* sess   = aegis_coding_agent_session(agent);
            aegis_session_t* forked = NULL;
            if (aegis_session_fork(sess, &forked) == AEGIS_OK) {
                char path[1024];
                snprintf(path, sizeof(path), ".aegis/session-%s.jsonl", aegis_session_id(forked));
                ensure_parent_dir(path);
                aegis_session_save(forked, path);
                printf("forked to %s (%s)\n", aegis_session_id(forked), path);
                aegis_session_destroy(forked);
            } else {
                printf("fork failed\n");
            }
            continue;
        }
        if (strcmp(line, "/compact") == 0) {
            aegis_session_t* sess   = aegis_coding_agent_session(agent);
            size_t           before = aegis_session_message_count(sess);
            st                      = aegis_session_compact(sess, 32);
            if (st == AEGIS_OK) {
                printf("compacted session: %zu -> %zu messages\n", before,
                       aegis_session_message_count(sess));
            } else {
                printf("compact failed: %s\n", aegis_status_str(st));
            }
            continue;
        }
        if (strcmp(line, "/json") == 0) {
            json_mode = !json_mode;
            printf("json mode %s\n", json_mode ? "on" : "off");
            continue;
        }
        if (strcmp(line, "/approvals") == 0 || strncmp(line, "/approvals ", 11) == 0) {
            const char* arg = line[10] == ' ' ? line + 11 : "";
            if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
                stream_ctx.approvals = (arg[0] == 'o' && arg[1] == 'n');
                printf("approvals %s\n", stream_ctx.approvals ? "on" : "off");
            } else if (*arg == '\0') {
                printf("approvals %s, always-allowed:",
                       stream_ctx.approvals ? "on" : "off");
                for (size_t i = 0; i < stream_ctx.allowed_count; i++) {
                    printf(" %s", stream_ctx.allowed_tools[i]);
                }
                printf("\n");
            } else {
                printf("usage: /approvals [on|off]\n");
            }
            continue;
        }
        if (strcmp(line, "/stream") == 0 || strncmp(line, "/stream ", 8) == 0) {
            const char* arg = line[7] == ' ' ? line + 8 : NULL;
            if (arg && strcmp(arg, "on") == 0) {
                stream_ctx.enabled = true;
            } else if (arg && strcmp(arg, "off") == 0) {
                stream_ctx.enabled = false;
            }
            printf("stream %s\n", stream_ctx.enabled ? "on" : "off");
            continue;
        }
        if (json_mode) {
            printf("{\"type\":\"user\",\"content\":\"");
        } else {
            printf("assistant:\n");
        }
        stream_ctx.text_emitted = false;
        stream_ctx.line_open    = false;
        /* Watch for interrupts / queued lines while the turn runs. */
        watcher_ctx_t w = {.agent = agent, .pending = NULL, .n = 0, .cap = 0,
                           .interrupted = false};
        g_gate_watcher = &w;
        pthread_t watcher;
        bool      watcher_up = pthread_create(&watcher, NULL, watcher_main, &w) == 0;
        aegis_status_t st2   = aegis_coding_agent_run(agent, line);
        if (watcher_up) {
            lq_close(&g_lines); /* wake the watcher if blocked */
            pthread_join(watcher, NULL);
            lq_init(&g_lines); /* reopen for the next turn */
            g_gate_watcher = NULL;
        }
        free(line);
        cli_stream_prelude(&stream_ctx);
        if (st2 == AEGIS_ERR_CANCELLED) {
            printf("⏹ interrupted\n");
        }
        if (st2 == AEGIS_OK) {
            aegis_session_t* sess = aegis_coding_agent_session(agent);
            size_t           n    = aegis_session_message_count(sess);
            if (n > 0) {
                const aegis_message_t* last    = aegis_session_message_at(sess, n - 1);
                const char*            content = aegis_message_content(last);
                /* Streamed-first: skip reprint when tokens were already shown. */
                if (content && (!stream_ctx.text_emitted || json_mode)) {
                    if (json_mode) {
                        for (const char* p = content; *p; p++) {
                            if (*p == '"') {
                                printf("\\\"");
                            } else if (*p == '\n') {
                                printf("\\n");
                            } else {
                                putchar(*p);
                            }
                        }
                        printf("\"}\n");
                    } else {
                        printf("%s\n", content);
                    }
                }
            }
            if (!json_mode) {
                printf("\nDone.\n");
            }
        } else if (st2 != AEGIS_ERR_CANCELLED) {
            printf("error: %s\n", aegis_status_str(st2));
        }
        /* Drain lines queued during the turn: each runs through the same
         * REPL logic by pushing them back as the next inputs. */
        if (watcher_up) {
            for (size_t i = 0; i < w.n; i++) {
                lq_push(&g_lines, w.pending[i]);
            }
            free(w.pending);
        }
    }
    if (reader_up) {
        lq_close(&g_lines);
        pthread_join(reader, NULL);
    }
    aegis_session_t* sess = aegis_coding_agent_session(agent);
    if (sess) {
        char path[1024];
        snprintf(path, sizeof(path), ".aegis/session-%s.jsonl", aegis_session_id(sess));
        ensure_parent_dir(path);
        aegis_session_save(sess, path);
        printf("session saved to %s\n", path);
    }
    aegis_coding_agent_destroy(agent);
    return 0;
}

int cmd_print(const char* prompt, const char* project_root, const char* model)
{
    aegis_coding_agent_config_t cfg = {0};
    cfg.project_root                = project_root ? project_root : ".";
    cfg.model                       = model ? model : "mock";
    cfg.provider                    = getenv("AEGIS_PROVIDER");
    cfg.api_key                     = getenv("OPENAI_API_KEY");
    cfg.base_url                    = getenv("AEGIS_OPENAI_BASE_URL");
    aegis_coding_agent_t* agent     = NULL;
    aegis_status_t        st        = aegis_coding_agent_create(&cfg, &agent);
    if (st != AEGIS_OK) {
        fprintf(stderr, "failed to create agent: %s\n", aegis_status_str(st));
        return 1;
    }
    st = aegis_coding_agent_run(agent, prompt);
    if (st == AEGIS_OK) {
        aegis_session_t* sess = aegis_coding_agent_session(agent);
        size_t           n    = aegis_session_message_count(sess);
        if (n > 0) {
            const aegis_message_t* last = aegis_session_message_at(sess, n - 1);
            printf("%s\n", aegis_message_content(last) ? aegis_message_content(last) : "");
        }
    } else {
        fprintf(stderr, "error: %s\n", aegis_status_str(st));
    }
    aegis_coding_agent_destroy(agent);
    return st == AEGIS_OK ? 0 : 1;
}
