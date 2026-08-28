/**
 * @file cli_helpers.h
 * @brief Shared CLI utilities: config parsing, path helpers.
 */
#ifndef AEGIS_CLI_HELPERS_H
#define AEGIS_CLI_HELPERS_H

#include "aegis/autonomous_agent.h"
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/status.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AEGIS_VERSION_STRING
#define AEGIS_VERSION_STRING "0.1.0"
#endif

#define DEFAULT_GOAL       "hello world"
#define DEFAULT_CHECKPOINT ".aegis/checkpoint.bin"
#define DEFAULT_LLM        "llm-mock"
#define DEFAULT_MAX_ITER   5u
#define DEFAULT_CONFIG     "aegis.conf"
#define PIDFILE            ".aegis/run.pid"

typedef struct cli_config {
    char     goal[512];
    char     checkpoint_path[512];
    char     llm_provider[64];
    char     config_path[512];
    uint32_t max_iterations;
    uint64_t timeout_ms;
} cli_config_t;

/**
 * @file cli.c
 * @brief Minimal Aegis CLI (Application layer, public API only).
 *
 * Commands:
 *   aegis init [--path DIR] [--force]
 *   aegis run [--goal TEXT] [--config PATH] [--max-iterations N] [--timeout MS]
 *   aegis status [--config PATH]
 *   aegis cancel [--config PATH]
 *   aegis inspect [--config PATH] [--checkpoint PATH]
 *   aegis --help / --version
 *
 * Design constraints:
 *   - CLI does NOT enter Core (no src/internal includes).
 *   - Only public headers under include/aegis.
 *   - No Runtime logic duplication: run delegates to aegis_autonomous_agent.
 *   - Config file support via CLI-local key=value parser (not Core config).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "aegis/autonomous_agent.h"
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/status.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AEGIS_VERSION_STRING
#define AEGIS_VERSION_STRING "0.1.0"
#endif

#define DEFAULT_GOAL       "hello world"
#define DEFAULT_CHECKPOINT ".aegis/checkpoint.bin"
#define DEFAULT_LLM        "llm-mock"
#define DEFAULT_MAX_ITER   5u
#define DEFAULT_CONFIG     "aegis.conf"
#define PIDFILE            ".aegis/run.pid"

typedef struct cli_config {
    char     goal[512];
    char     checkpoint_path[512];
    char     llm_provider[64];
    char     config_path[512];
    uint32_t max_iterations;
    uint64_t timeout_ms;
} cli_config_t;

static void cli_config_default(cli_config_t* c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->goal, sizeof(c->goal), "%s", DEFAULT_GOAL);
    snprintf(c->checkpoint_path, sizeof(c->checkpoint_path), "%s", DEFAULT_CHECKPOINT);
    snprintf(c->llm_provider, sizeof(c->llm_provider), "%s", DEFAULT_LLM);
    snprintf(c->config_path, sizeof(c->config_path), "%s", DEFAULT_CONFIG);
    c->max_iterations = DEFAULT_MAX_ITER;
    c->timeout_ms     = 0;
}

static void trim(char* s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    char* p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static int cli_config_load(cli_config_t* c, const char* path)
{
    FILE* fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            return 0;  // not found -> keep defaults
        }
        fprintf(stderr, "error: cannot open config '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq     = '\0';
        char* k = line;
        char* v = eq + 1;
        trim(k);
        trim(v);
        if (strcmp(k, "goal") == 0) {
            snprintf(c->goal, sizeof(c->goal), "%s", v);
        } else if (strcmp(k, "checkpoint_path") == 0) {
            snprintf(c->checkpoint_path, sizeof(c->checkpoint_path), "%s", v);
        } else if (strcmp(k, "llm_provider") == 0) {
            snprintf(c->llm_provider, sizeof(c->llm_provider), "%s", v);
        } else if (strcmp(k, "max_iterations") == 0) {
            c->max_iterations = (uint32_t)strtoul(v, NULL, 10);
            if (c->max_iterations == 0) {
                c->max_iterations = DEFAULT_MAX_ITER;
            }
        } else if (strcmp(k, "timeout_ms") == 0) {
            c->timeout_ms = (uint64_t)strtoull(v, NULL, 10);
        } else if (strcmp(k, "config_path") == 0) {
            // ignore
        }
    }
    fclose(fp);
    return 0;
}

static int cli_config_save(const cli_config_t* c, const char* path)
{
    FILE* fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "error: cannot write config '%s': %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(fp, "# Aegis config (key=value)\n");
    fprintf(fp, "goal=%s\n", c->goal);
    fprintf(fp, "checkpoint_path=%s\n", c->checkpoint_path);
    fprintf(fp, "llm_provider=%s\n", c->llm_provider);
    fprintf(fp, "max_iterations=%u\n", c->max_iterations);
    fprintf(fp, "timeout_ms=%llu\n", (unsigned long long)c->timeout_ms);
    fclose(fp);
    return 0;
}

static int mkdir_p(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void ensure_parent_dir(const char* filepath)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char* slash = strrchr(dir, '/');
    if (!slash) {
        return;
    }
    *slash = '\0';
    if (dir[0] == '\0') {
        return;
    }
    mkdir_p(dir);
}

static void print_usage(FILE* out)
{
    fprintf(out,
            "Usage: aegis <command> [options]\n"
            "\n"
            "Commands:\n"
            "  init [--path DIR] [--force]             create config and .aegis dir\n"
            "  run [--goal TEXT] [--config PATH] [--max-iterations N] [--timeout MS]\n"
            "                                           run autonomous agent (delegates to Core)\n"
            "  status [--config PATH]                   show checkpoint status\n"
            "  cancel [--config PATH]                   cancel running agent (pidfile)\n"
            "  inspect [--config PATH] [--checkpoint PATH]\n"
            "                                           dump checkpoint details\n"
            "\n"
            "Global:\n"
            "  --help, -h     show this help\n"
            "  --version, -v  show version\n"
            "\n"
            "Config file (default: ./aegis.conf, key=value):\n"
            "  goal, checkpoint_path, llm_provider, max_iterations, timeout_ms\n");
}

static void print_version(void)
{
    printf("aegis %s\n", AEGIS_VERSION_STRING);
}

/* ── init ──────────────────────────────────────────────────────────────────── */


/* Command prototypes */
extern int cmd_init(int argc, char** argv);
extern int cmd_run(int argc, char** argv);
extern int cmd_status(int argc, char** argv);
extern int cmd_cancel(int argc, char** argv);
extern int cmd_inspect(int argc, char** argv);

#endif /* AEGIS_CLI_HELPERS_H */
