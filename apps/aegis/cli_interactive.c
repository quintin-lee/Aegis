#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "aegis/coding/coding_agent.h"
#include "aegis/session/session.h"
#include <dirent.h>
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
    static cli_stream_ctx_t stream_ctx = {.enabled = true, .text_emitted = false, .line_open = false};
    if (!json_mode_env()) {
        aegis_coding_agent_set_event_callback(agent, cli_event_cb, &stream_ctx);
    }
    char        line[4096];
    int         json_mode = json_mode_env();
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (strcmp(line, "/help") == 0 || strcmp(line, "/h") == 0) {
            printf("/help /model /session /sessions /resume /fork /tree /compact /json /stream /clear /quit\n");
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
        aegis_status_t st2 = aegis_coding_agent_run(agent, line);
        cli_stream_prelude(&stream_ctx);
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
        } else if (st2 == AEGIS_ERR_CANCELLED) {
            printf("cancelled\n");
        } else {
            printf("error: %s\n", aegis_status_str(st2));
        }
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
