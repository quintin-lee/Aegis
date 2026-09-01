/**
 * @file cli_run.c
 * @brief aegis run command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

#ifdef AEGIS_OPENAI_PROVIDER
#include "aegis/provider/openai_llm.h"
#endif

int cmd_run(int argc, char** argv)
{
    cli_config_t cfg;
    cli_config_default(&cfg);
    const char* explicit_goal   = NULL;
    const char* config_path     = NULL;
    const char* timeout_str     = NULL;
    const char* iter_str        = NULL;
    const char* provider_name   = NULL;
    const char* model_name      = NULL;
    const char* api_key         = NULL;
    const char* base_url        = NULL;
#define UNUSED(x) (void)(x)


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
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_name = argv[++i];
        } else if (strncmp(argv[i], "--provider=", 11) == 0) {
            provider_name = argv[i] + 11;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_name = argv[++i];
        } else if (strncmp(argv[i], "--model=", 8) == 0) {
            model_name = argv[i] + 8;
        } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            api_key = argv[++i];
        } else if (strncmp(argv[i], "--api-key=", 10) == 0) {
            api_key = argv[i] + 10;
        } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
            base_url = argv[++i];
        } else if (strncmp(argv[i], "--base-url=", 11) == 0) {
            base_url = argv[i] + 11;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf(
                "Usage: aegis run [--goal TEXT] [--config PATH] [--max-iterations N] [--timeout "
                "MS]\n"
                "                [--provider NAME] [--model MODEL] [--api-key KEY] "
                "[--base-url URL]\n"
                "                [--help]\n"
                "\n"
                "Options:\n"
                "  --goal TEXT        Agent goal / prompt\n"
                "  --config PATH      Config file path (default: aegis.conf)\n"
                "  --max-iterations N Maximum autonomous iterations (default: 5)\n"
                "  --timeout MS       Per-task timeout in milliseconds (default: 0 = unlimited)\n"
                "  --provider NAME    LLM provider name: llm-mock (default) or llm-openai\n"
                "  --model MODEL      Model id (default: gpt-4o-mini for openai, mock for mock)\n"
                "  --api-key KEY      OpenAI API key (or set OPENAI_API_KEY env var)\n"
                "  --base-url URL     OpenAI-compatible base URL (or set AEGIS_OPENAI_BASE_URL)\n"
                "  --help, -h         Show this help\n");
            return 0;
        } else {
            fprintf(stderr, "error: unknown option for run: '%s'\n", argv[i]);
            fprintf(stderr,
                    "Try 'aegis run --help' for usage.\n"
                    "Options: [--goal TEXT] [--config PATH] [--max-iterations N] [--timeout "
                    "MS]\n"
                    "         [--provider NAME] [--model MODEL] [--api-key KEY] [--base-url "
                    "URL]\n");
            return 1;
        }
    }
    if (explicit_goal) {
        snprintf(cfg.goal, sizeof(cfg.goal), "%s", explicit_goal);
    }
    if (iter_str) {
        char* end = NULL;
        unsigned long n = strtoul(iter_str, &end, 10);
        if (end && *end == '\0' && n > 0 && n <= 10000) {
            cfg.max_iterations = (uint32_t)n;
        } else {
            fprintf(stderr, "error: invalid --max-iterations value: '%s'\n", iter_str);
            return 1;
        }
    }
    if (timeout_str) {
        char* end = NULL;
        unsigned long ms = strtoul(timeout_str, &end, 10);
        if (end && *end == '\0' && ms <= 3600000) {
            cfg.timeout_ms = (uint64_t)ms;
        } else {
            fprintf(stderr, "error: invalid --timeout value: '%s'\n", timeout_str);
            return 1;
        }
    }
    if (provider_name) {
        snprintf(cfg.llm_provider, sizeof(cfg.llm_provider), "%s", provider_name);
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

    // Create and register the selected LLM provider
    const char* llm_name    = cfg.llm_provider;
    void*       llm_ctx     = NULL;
    aegis_provider_def_t llm_def;
    memset(&llm_def, 0, sizeof(llm_def));

    if (strcmp(llm_name, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
        openai_llm_ctx_t* octx = NULL;
        const aegis_llm_ops_t* ops = NULL;
        rc = aegis_openai_llm_create(&octx, &ops, &llm_def);
        if (rc != AEGIS_OK) {
            fprintf(stderr, "error: openai llm create failed: %s\n", aegis_status_str(rc));
            aegis_provider_registry_destroy(reg);
            return 1;
        }
        // apply overrides (cli > env > default)
        aegis_openai_llm_configure(octx, api_key, base_url, model_name);
        llm_ctx = octx;
#else
        fprintf(stderr, "error: llm-openai provider not compiled in (rebuild with "
                        "-DAEGIS_OPENAI_PROVIDER=ON)\n");
        aegis_provider_registry_destroy(reg);
        return 1;
#endif
    } else if (strcmp(llm_name, "llm-mock") == 0 || llm_name[0] == '\0') {
        llm_mock_ctx_t* mock_ctx = NULL;
        const aegis_llm_ops_t* ops = NULL;
        rc = aegis_llm_mock_create(&mock_ctx, &ops, &llm_def);
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
        llm_ctx = mock_ctx;
    } else {
        fprintf(stderr, "error: unknown llm provider '%s' (supported: llm-mock, llm-openai)\n",
                llm_name);
        aegis_provider_registry_destroy(reg);
        return 1;
    }

    rc = aegis_provider_register(reg, &llm_def);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: provider register failed: %s\n", aegis_status_str(rc));
        if (strcmp(cfg.llm_provider, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
            aegis_openai_llm_destroy((openai_llm_ctx_t*)llm_ctx, NULL);
#endif
        } else {
            aegis_llm_mock_destroy((llm_mock_ctx_t*)llm_ctx, NULL);
        }
        aegis_provider_registry_destroy(reg);
        return 1;
    }
    rc = aegis_provider_init(reg, llm_def.name);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: provider init failed: %s\n", aegis_status_str(rc));
        aegis_provider_unregister(reg, llm_def.name);
        if (strcmp(cfg.llm_provider, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
            aegis_openai_llm_destroy((openai_llm_ctx_t*)llm_ctx, NULL);
#endif
        } else {
            aegis_llm_mock_destroy((llm_mock_ctx_t*)llm_ctx, NULL);
        }
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
        .llm_provider_name       = llm_def.name,
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
        aegis_provider_unregister(reg, llm_def.name);
        if (strcmp(cfg.llm_provider, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
            aegis_openai_llm_destroy((openai_llm_ctx_t*)llm_ctx, NULL);
#endif
        } else {
            aegis_llm_mock_destroy((llm_mock_ctx_t*)llm_ctx, NULL);
        }
        aegis_provider_registry_destroy(reg);
        return 1;
    }
    aegis_autonomous_result_t result;
    memset(&result, 0, sizeof(result));
    rc = aegis_autonomous_agent_run(aa, cfg.goal, &result);

    aegis_autonomous_agent_destroy(aa);
    aegis_provider_unregister(reg, llm_def.name);
    if (strcmp(cfg.llm_provider, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
        aegis_openai_llm_destroy((openai_llm_ctx_t*)llm_ctx, NULL);
#endif
    } else {
        aegis_llm_mock_destroy((llm_mock_ctx_t*)llm_ctx, NULL);
    }
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
    UNUSED(model_name);
    UNUSED(api_key);
    UNUSED(base_url);
     fprintf(stderr, "error: run failed: %s\n", aegis_status_str(rc));
     return 1;
 }

/* ── status ────────────────────────────────────────────────────────────────── */
