#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "cli_repl.h"
#include "aegis/coding/coding_agent.h"
#include "aegis/session/session.h"
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

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
    static cli_stream_ctx_t stream_ctx = {.enabled       = true,
                                          .text_emitted  = false,
                                          .line_open     = false,
                                          .approvals     = false,
                                          .allowed_count = 0};
    aegis_coding_agent_set_tool_approval(agent, cli_approval_cb, &stream_ctx);
    if (!json_mode_env()) {
        aegis_coding_agent_set_event_callback(agent, cli_event_cb, &stream_ctx);
    }
    int json_mode = json_mode_env();
    raw_enable();
    lq_init(&g_lines);
    pthread_t reader;
    bool      reader_up = pthread_create(&reader, NULL, reader_main, NULL) == 0;
    if (reader_up) {
        pthread_detach(reader); /* quit must not block on a PTY (no EOF) */
    }
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
            printf(
                "/help /model /tools /usage /session /sessions /resume /fork /tree /compact /json "
                "/stream /approvals /stop /clear /quit\n");
            continue;
        }
        if (strcmp(line, "/usage") == 0) {
            aegis_usage_t ulast = {0}, utotal = {0};
            if (aegis_coding_agent_usage(agent, &ulast, &utotal) != AEGIS_OK) {
                printf("error: usage unavailable\n");
                continue;
            }
            printf("last turn: in %u · out %u · total %u\n", ulast.input_tokens,
                   ulast.output_tokens, ulast.total_tokens);
            printf("session:   in %u · out %u · total %u\n", utotal.input_tokens,
                   utotal.output_tokens, utotal.total_tokens);
            continue;
        }
        if (strcmp(line, "/tools") == 0) {
            aegis_tool_registry_t* tools = NULL;
            aegis_status_t         stt   = aegis_coding_agent_tools(agent, &tools);
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
                if (strncmp(n, "session-", 8) != 0 || l < 8 + 4 ||
                    strcmp(n + l - 6, ".jsonl") != 0) {
                    continue;
                }
                char pbuf[1024];
                snprintf(pbuf, sizeof(pbuf), ".aegis/%s", n);
                struct stat st;
                if (stat(pbuf, &st) != 0) {
                    continue;
                }
                char      tsbuf[32];
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
        if (strcmp(line, "/stop") == 0) {
            /* No turn is running here; the watcher handles /stop mid-turn. */
            printf("not running\n");
            free(line);
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
                printf("approvals %s, always-allowed:", stream_ctx.approvals ? "on" : "off");
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
        watcher_ctx_t w = {.agent = agent, .pending = NULL, .n = 0, .cap = 0, .interrupted = false};
        g_gate_watcher  = &w;
        pthread_t      watcher;
        bool           watcher_up = pthread_create(&watcher, NULL, watcher_main, &w) == 0;
        aegis_status_t st2        = aegis_coding_agent_run(agent, line);
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
            if (!json_mode) {
                aegis_usage_t ulast = {0}, utotal = {0};
                if (aegis_coding_agent_usage(agent, &ulast, &utotal) == AEGIS_OK &&
                    utotal.total_tokens > 0) {
                    printf("tokens: in %u · out %u · total %u (session %u)\n", ulast.input_tokens,
                           ulast.output_tokens, ulast.total_tokens, utotal.total_tokens);
                }
            }
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
        /* Release the poll-waiting reader, then let it exit on its own;
         * joining would hang under a PTY, where stdin never EOFs. */
        g_reader_shutdown = 1;
        lq_close(&g_lines);
    }
    raw_disable();
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
