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

void cli_config_default(cli_config_t* c);

void trim(char* s);

int cli_config_load(cli_config_t* c, const char* path);

int cli_config_save(const cli_config_t* c, const char* path);

int mkdir_p(const char* path);

void ensure_parent_dir(const char* filepath);

void print_usage(FILE* out);

void print_version(void);

/* ── init ──────────────────────────────────────────────────────────────────── */

/* Command prototypes */
extern int cmd_init(int argc, char** argv);
extern int cmd_run(int argc, char** argv);
extern int cmd_status(int argc, char** argv);
extern int cmd_cancel(int argc, char** argv);
extern int cmd_inspect(int argc, char** argv);

#endif /* AEGIS_CLI_HELPERS_H */
