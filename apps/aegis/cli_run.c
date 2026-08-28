/**
 * @file cli_run.c
 * @brief aegis run command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

int cmd_run(int argc, char** argv)
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
