#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "aegis/coding/coding_agent.h"
#include "aegis/session/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_banner(void)
{
    printf("Aegis Coding Agent\n");
    printf("project: %s\n", ".");
    printf("model: mock\n");
    printf("type /help for commands\n\n");
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
    print_banner();
    char        line[4096];
    int         json_mode = 0;
    const char* env_json  = getenv("AEGIS_JSON");
    if (env_json && strcmp(env_json, "1") == 0) {
        json_mode = 1;
    }
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
            printf("/help /model /session /sessions /resume /fork /tree /compact /clear /quit\n");
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
        if (strcmp(line, "/tree") == 0) {
            aegis_session_t* sess = aegis_coding_agent_session(agent);
            printf("session %s branch %s parent %s messages %zu\n", aegis_session_id(sess),
                   aegis_session_branch_id(sess),
                   aegis_session_parent_id(sess) ? aegis_session_parent_id(sess) : "-",
                   aegis_session_message_count(sess));
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
        if (json_mode) {
            printf("{\"type\":\"user\",\"content\":\"");
        } else {
            printf("assistant:\n");
        }
        aegis_status_t st2 = aegis_coding_agent_run(agent, line);
        if (st2 == AEGIS_OK) {
            aegis_session_t* sess = aegis_coding_agent_session(agent);
            size_t           n    = aegis_session_message_count(sess);
            if (n > 0) {
                const aegis_message_t* last    = aegis_session_message_at(sess, n - 1);
                const char*            content = aegis_message_content(last);
                if (content) {
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
