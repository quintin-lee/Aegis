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
#include "aegis/checkpoint.h"
#include "aegis/provider.h"
#include "aegis/provider_llm_mock.h"
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

static int cmd_init(int argc, char** argv)
{
    const char* base  = ".";
    int         force = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            base = argv[++i];
        } else if (strncmp(argv[i], "--path=", 7) == 0) {
            base = argv[i] + 7;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: aegis init [--path DIR] [--force]\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for init: '%s'\n", argv[i]);
            fprintf(stderr, "Usage: aegis init [--path DIR] [--force]\n");
            return 1;
        }
    }
    char config_path[1024];
    if (strcmp(base, ".") == 0) {
        snprintf(config_path, sizeof(config_path), "%s", DEFAULT_CONFIG);
    } else {
        snprintf(config_path, sizeof(config_path), "%s/%s", base, DEFAULT_CONFIG);
        if (mkdir_p(base) != 0 && errno != EEXIST) {
            fprintf(stderr, "error: cannot create dir '%s': %s\n", base, strerror(errno));
            return 1;
        }
    }
    if (!force && access(config_path, F_OK) == 0) {
        fprintf(stderr, "error: config already exists at '%s' (use --force to overwrite)\n",
                config_path);
        return 1;
    }
    char dot_aegis[1024];
    if (strcmp(base, ".") == 0) {
        snprintf(dot_aegis, sizeof(dot_aegis), ".aegis");
    } else {
        snprintf(dot_aegis, sizeof(dot_aegis), "%s/.aegis", base);
    }
    mkdir_p(dot_aegis);

    cli_config_t cfg;
    cli_config_default(&cfg);
    // adjust checkpoint path when base != .
    if (strcmp(base, ".") != 0) {
        snprintf(cfg.checkpoint_path, sizeof(cfg.checkpoint_path), "%s/.aegis/checkpoint.bin",
                 base);
    }
    if (cli_config_save(&cfg, config_path) != 0) {
        return 1;
    }
    printf("init ok: config '%s' created\n", config_path);
    return 0;
}

/* ── run ───────────────────────────────────────────────────────────────────── */

static int cmd_run(int argc, char** argv)
{
    cli_config_t cfg;
    cli_config_default(&cfg);
    const char* explicit_goal = NULL;
    const char* config_path   = NULL;
    const char* timeout_str   = NULL;
    const char* iter_str      = NULL;

    // first pass: find --config
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--config") == 0 && i + 1 < argc) ||
            strncmp(argv[i], "--config=", 9) == 0) {
            config_path = (strncmp(argv[i], "--config=", 9) == 0) ? argv[i] + 9 : argv[i + 1];
            snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", config_path);
        }
    }
    if (cli_config_load(&cfg, cfg.config_path) != 0) {
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
            explicit_goal = argv[++i];
        } else if (strncmp(argv[i], "--goal=", 7) == 0) {
            explicit_goal = argv[i] + 7;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            i++;  // already handled
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            // handled
        } else if (strcmp(argv[i], "--max-iterations") == 0 && i + 1 < argc) {
            iter_str = argv[++i];
        } else if (strncmp(argv[i], "--max-iterations=", 17) == 0) {
            iter_str = argv[i] + 17;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_str = argv[++i];
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_str = argv[i] + 10;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf(
                "Usage: aegis run [--goal TEXT] [--config PATH] [--max-iterations N] [--timeout "
                "MS]\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for run: '%s'\n", argv[i]);
            fprintf(stderr,
                    "Usage: aegis run [--goal TEXT] [--config PATH] [--max-iterations N] "
                    "[--timeout MS]\n");
            return 1;
        }
    }
    if (explicit_goal) {
        snprintf(cfg.goal, sizeof(cfg.goal), "%s", explicit_goal);
    }
    if (iter_str) {
        unsigned long v = strtoul(iter_str, NULL, 10);
        if (v == 0 || v > 100) {
            fprintf(stderr, "error: invalid --max-iterations '%s' (must be 1..100)\n", iter_str);
            return 1;
        }
        cfg.max_iterations = (uint32_t)v;
    }
    if (timeout_str) {
        long long v = atoll(timeout_str);
        if (v < 0) {
            fprintf(stderr, "error: invalid --timeout '%s'\n", timeout_str);
            return 1;
        }
        cfg.timeout_ms = (uint64_t)v;
    }
    if (cfg.goal[0] == '\0') {
        fprintf(stderr, "error: goal is required (provide --goal or set goal in config '%s')\n",
                cfg.config_path);
        return 1;
    }

    // ensure parent dir for checkpoint
    ensure_parent_dir(cfg.checkpoint_path);

    // delegate to autonomous_agent (no Runtime duplication)
    aegis_provider_registry_t* reg = NULL;
    aegis_status_t             rc  = aegis_provider_registry_create(&reg);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: provider registry create failed: %s\n", aegis_status_str(rc));
        return 1;
    }
    llm_mock_ctx_t*        mock_ctx = NULL;
    const aegis_llm_ops_t* ops      = NULL;
    aegis_provider_def_t   def;
    rc = aegis_llm_mock_create(&mock_ctx, &ops, &def);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: llm mock create failed: %s\n", aegis_status_str(rc));
        aegis_provider_registry_destroy(reg);
        return 1;
    }
    // provide a default canned DSL so run succeeds without external LLM
    const char* default_resp =
        "STEP|-1|computational||step1|do step1\n"
        "STEP|-1|computational||step2|do step2\n";
    aegis_llm_mock_set_response(mock_ctx, default_resp);

    rc = aegis_provider_register(reg, &def);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: provider register failed: %s\n", aegis_status_str(rc));
        aegis_llm_mock_destroy(mock_ctx, ops);
        aegis_provider_registry_destroy(reg);
        return 1;
    }
    rc = aegis_provider_init(reg, def.name);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: provider init failed: %s\n", aegis_status_str(rc));
        aegis_provider_unregister(reg, def.name);
        aegis_llm_mock_destroy(mock_ctx, ops);
        aegis_provider_registry_destroy(reg);
        return 1;
    }

    // write pidfile for cancel
    ensure_parent_dir(PIDFILE);
    FILE* pf = fopen(PIDFILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", (int)getpid());
        fclose(pf);
    }

    aegis_autonomous_agent_config_t acfg = {
        .provider_registry       = reg,
        .llm_provider_name       = def.name,
        .checkpoint_path         = cfg.checkpoint_path,
        .cancel_token            = NULL,
        .max_iterations          = cfg.max_iterations,
        .default_task_timeout_ns = cfg.timeout_ms * 1000000ULL,
    };
    aegis_autonomous_agent_t* aa = NULL;
    rc                           = aegis_autonomous_agent_create(&aa, &acfg);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: autonomous agent create failed: %s\n", aegis_status_str(rc));
        unlink(PIDFILE);
        aegis_provider_unregister(reg, def.name);
        aegis_llm_mock_destroy(mock_ctx, ops);
        aegis_provider_registry_destroy(reg);
        return 1;
    }
    aegis_autonomous_result_t result;
    memset(&result, 0, sizeof(result));
    rc = aegis_autonomous_agent_run(aa, cfg.goal, &result);

    aegis_autonomous_agent_destroy(aa);
    aegis_provider_unregister(reg, def.name);
    aegis_llm_mock_destroy(mock_ctx, ops);
    aegis_provider_registry_destroy(reg);
    unlink(PIDFILE);

    if (rc == AEGIS_OK) {
        printf("run ok: tasks=%u iterations=%u status=%s\n", result.tasks_executed,
               result.iterations, aegis_status_str(result.final_status));
        return 0;
    }
    if (rc == AEGIS_ERR_CANCELLED) {
        fprintf(stderr, "error: run cancelled\n");
        return 1;
    }
    if (rc == AEGIS_ERR_TIMEOUT) {
        fprintf(stderr, "error: run timed out: %s\n", aegis_status_str(rc));
        return 1;
    }
    fprintf(stderr, "error: run failed: %s\n", aegis_status_str(rc));
    return 1;
}

/* ── status ────────────────────────────────────────────────────────────────── */

static int cmd_status(int argc, char** argv)
{
    const char* config_path = DEFAULT_CONFIG;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: aegis status [--config PATH]\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for status: '%s'\n", argv[i]);
            return 1;
        }
    }
    cli_config_t cfg;
    cli_config_default(&cfg);
    snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", config_path);
    cli_config_load(&cfg, cfg.config_path);  // ignore missing

    const char* ckpt_path = cfg.checkpoint_path;
    // allow env override for tests
    const char* env = getenv("AEGIS_CHECKPOINT");
    if (env && env[0] != '\0') {
        ckpt_path = env;
    }

    aegis_checkpoint_t*       ckpt = NULL;
    aegis_checkpoint_status_t st   = AEGIS_CHECKPOINT_MISSING;
    aegis_status_t            rc   = aegis_checkpoint_read(ckpt_path, &ckpt, &st);
    if (rc == AEGIS_ERR_NOT_FOUND || st == AEGIS_CHECKPOINT_MISSING) {
        printf("status: no checkpoint at '%s'\n", ckpt_path);
        return 0;
    }
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: checkpoint corrupted at '%s': %s (%s)\n", ckpt_path,
                aegis_status_str(rc), aegis_checkpoint_status_str(st));
        if (ckpt) {
            aegis_checkpoint_destroy(ckpt);
        }
        return 1;
    }
    printf("checkpoint: %s\n", ckpt_path);
    printf("version: %u\n", aegis_checkpoint_version(ckpt));
    printf("agent_state: %s\n", aegis_checkpoint_agent_state(ckpt));
    printf("goal: %s\n", aegis_checkpoint_goal(ckpt));
    printf("plan_version: %u\n", aegis_checkpoint_plan_version(ckpt));
    printf("tasks: %zu\n", aegis_checkpoint_task_count(ckpt));
    for (size_t i = 0; i < aegis_checkpoint_task_count(ckpt); i++) {
        const aegis_checkpoint_task_snapshot_t* t = aegis_checkpoint_task_snapshot(ckpt, i);
        if (t) {
            printf("  task[%zu] id=%u name=%s state=%d\n", i, t->task_id, t->task_name,
                   t->task_state);
        }
    }
    // pidfile hint
    if (access(PIDFILE, F_OK) == 0) {
        printf("run: active (pidfile present)\n");
    } else {
        printf("run: idle\n");
    }
    aegis_checkpoint_destroy(ckpt);
    return 0;
}

/* ── cancel ────────────────────────────────────────────────────────────────── */

static int cmd_cancel(int argc, char** argv)
{
    const char* config_path = DEFAULT_CONFIG;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: aegis cancel [--config PATH]\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for cancel: '%s'\n", argv[i]);
            return 1;
        }
    }
    (void)config_path;
    if (access(PIDFILE, F_OK) != 0) {
        fprintf(stderr, "error: no running agent found (no pidfile at '%s')\n", PIDFILE);
        return 1;
    }
    FILE* pf = fopen(PIDFILE, "r");
    if (!pf) {
        fprintf(stderr, "error: cannot read pidfile '%s': %s\n", PIDFILE, strerror(errno));
        return 1;
    }
    int pid = 0;
    if (fscanf(pf, "%d", &pid) != 1 || pid <= 0) {
        fclose(pf);
        fprintf(stderr, "error: invalid pidfile content at '%s'\n", PIDFILE);
        return 1;
    }
    fclose(pf);
    // Try to signal. If process already gone, treat as cancelled.
    if (kill(pid, 0) != 0 && errno == ESRCH) {
        unlink(PIDFILE);
        printf("cancelled: no process %d (already exited)\n", pid);
        return 0;
    }
    // Best-effort: send SIGTERM, then unlink. autonomous_agent will observe cancellation via
    // signal? For now pidfile removal is sufficient for CLI contract. We also write a cancel
    // sentinel that a running loop could poll via file existence (future).
    unlink(PIDFILE);
    // Try to terminate
    kill(pid, 15);
    printf("cancelled: signalled pid %d\n", pid);
    return 0;
}

/* ── inspect ───────────────────────────────────────────────────────────────── */

static int cmd_inspect(int argc, char** argv)
{
    const char* config_path = DEFAULT_CONFIG;
    const char* ckpt_path   = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc) {
            ckpt_path = argv[++i];
        } else if (strncmp(argv[i], "--checkpoint=", 13) == 0) {
            ckpt_path = argv[i] + 13;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: aegis inspect [--config PATH] [--checkpoint PATH]\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for inspect: '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!ckpt_path) {
        cli_config_t cfg;
        cli_config_default(&cfg);
        snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", config_path);
        cli_config_load(&cfg, cfg.config_path);
        ckpt_path       = cfg.checkpoint_path;
        const char* env = getenv("AEGIS_CHECKPOINT");
        if (env && env[0] != '\0') {
            ckpt_path = env;
        }
        // copy to static buffer to avoid dangling pointer to cfg
        static char buf[1024];
        snprintf(buf, sizeof(buf), "%s", ckpt_path);
        ckpt_path = buf;
    }
    aegis_checkpoint_t*       ckpt = NULL;
    aegis_checkpoint_status_t st   = AEGIS_CHECKPOINT_MISSING;
    aegis_status_t            rc   = aegis_checkpoint_read(ckpt_path, &ckpt, &st);
    if (rc == AEGIS_ERR_NOT_FOUND || st == AEGIS_CHECKPOINT_MISSING) {
        fprintf(stderr, "error: no checkpoint at '%s'\n", ckpt_path);
        return 1;
    }
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: checkpoint corrupted at '%s': %s (%s)\n", ckpt_path,
                aegis_status_str(rc), aegis_checkpoint_status_str(st));
        if (ckpt) {
            aegis_checkpoint_destroy(ckpt);
        }
        return 1;
    }
    char* dump = NULL;
    if (aegis_checkpoint_serialize(ckpt, &dump) == AEGIS_OK && dump) {
        printf("%s", dump);
        free(dump);
    } else {
        printf("checkpoint: %s\n", ckpt_path);
        printf("version: %u\n", aegis_checkpoint_version(ckpt));
        printf("agent_state: %s\n", aegis_checkpoint_agent_state(ckpt));
        printf("goal: %s\n", aegis_checkpoint_goal(ckpt));
        const char* pt = aegis_checkpoint_plan_text(ckpt);
        if (pt) {
            printf("plan:\n%s\n", pt);
        }
    }
    aegis_checkpoint_destroy(ckpt);
    return 0;
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }
    const char* cmd = argv[1];
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "version") == 0) {
        print_version();
        return 0;
    }
    int    sub_argc = argc - 2;
    char** sub_argv = argv + 2;
    if (strcmp(cmd, "init") == 0) {
        return cmd_init(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "status") == 0) {
        return cmd_status(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "cancel") == 0) {
        return cmd_cancel(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "inspect") == 0) {
        return cmd_inspect(sub_argc, sub_argv);
    }
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    print_usage(stderr);
    return 1;
}
