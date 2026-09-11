/**
 * @file cli_render.c
 * @brief Live terminal rendering: streaming text/reasoning deltas,
 * per-tool timing lines, banner, and /tools listing. Pure output layer —
 * no agent, session, or input state touched.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_repl.h"
#include "aegis/status.h"
#include "aegis/tool/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void print_banner(const char* model)
{
    printf("Aegis Coding Agent\n");
    printf("project: %s\n", ".");
    printf("model: %s\n", model ? model : "mock");
    printf("type /help for commands\n\n");
}

/* ── Live streaming output ────────────────────────────────────────── */

void cli_stream_prelude(cli_stream_ctx_t* cx)
{
    if (cx->line_open) {
        putchar('\n');
        cx->line_open = false;
    }
}

/* AEGIS_JSON=1 forces machine-readable output: no live streaming callback. */
bool json_mode_env(void)
{
    const char* env = getenv("AEGIS_JSON");
    return env && strcmp(env, "1") == 0;
}

/* /tools visitor: print "name — description" plus the parameter list. */
static const char* cli_val_type_str(aegis_tool_value_type_t t)
{
    switch (t) {
    case AEGIS_TOOL_VAL_STRING:
        return "string";
    case AEGIS_TOOL_VAL_INT:
        return "int";
    case AEGIS_TOOL_VAL_FLOAT:
        return "float";
    case AEGIS_TOOL_VAL_BOOL:
        return "bool";
    default:
        return "bytes";
    }
}

aegis_status_t cli_print_tool_def(const aegis_tool_def_t* def, void* user)
{
    (void)user;
    printf("  %s — %s\n", def->name, def->description ? def->description : "(no description)");
    for (size_t i = 0; def->schema.params && i < def->schema.param_count; i++) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (p->description) {
            printf("    %s%s: %s (%s)\n", p->name, p->required ? "" : "?",
                   cli_val_type_str(p->type), p->description);
        } else {
            printf("    %s%s: %s\n", p->name, p->required ? "" : "?", cli_val_type_str(p->type));
        }
    }
    return AEGIS_OK;
}

void cli_event_cb(const aegis_agent_event_t* ev, void* user)
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
