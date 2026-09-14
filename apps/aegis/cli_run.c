/**
 * @file cli_run.c
 * @brief aegis run command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "aegis/coding/coding_agent.h"

#ifdef AEGIS_OPENAI_PROVIDER
#include "aegis/provider/openai_llm.h"
#endif

static void save_session_best_effort(aegis_coding_agent_t* ca, const char* checkpoint_path)
{
    if (!ca || !checkpoint_path) {
        return;
    }
    aegis_session_t* sess = aegis_coding_agent_session(ca);
    if (!sess) {
        return;
    }
    char sess_path[1024];
    cli_session_path_for_checkpoint(checkpoint_path, sess_path, sizeof(sess_path));
    ensure_parent_dir(sess_path);
    aegis_status_t src = aegis_session_save(sess, sess_path);
    if (src != AEGIS_OK) {
        fprintf(stderr, "warning: session save failed: %s\n", aegis_status_str(src));
    }
}

/**
 * @brief One-shot run command: load config, create the coding agent, run
 *        with an optional explicit goal, and save the session at exit.
 *
 * Parses --config/--goal/--timeout/--iter/--provider/--model/--api-key/
 * --base-url flags (precedence: explicit > config-file > defaults). Builds
 * the agent, drives a single run(), and best-effort persists the session
 * back to the configured checkpoint path. Returns 0 on success, 1 on
 * failure.
 *
 * @param argc Argument count (from main).
 * @param argv Argument vector (from main).
 * @return 0 on success, 1 on failure.
 */
int cmd_run(int argc, char** argv)
{
    cli_config_t cfg;
    cli_config_default(&cfg);
    const char* explicit_goal = NULL;
    const char* config_path   = NULL;
    const char* timeout_str   = NULL;
    const char* iter_str      = NULL;
    const char* provider_name = NULL;
    const char* model_name    = NULL;
    const char* api_key       = NULL;
    const char* base_url      = NULL;
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
        char*         end = NULL;
        unsigned long n   = strtoul(iter_str, &end, 10);
        if (end && *end == '\0' && n > 0 && n <= 10000) {
            cfg.max_iterations = (uint32_t)n;
        } else {
            fprintf(stderr, "error: invalid --max-iterations value: '%s'\n", iter_str);
            return 1;
        }
    }
    if (timeout_str) {
        char*         end = NULL;
        unsigned long ms  = strtoul(timeout_str, &end, 10);
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

    if (strcmp(cfg.llm_provider, "llm-mock") != 0 && strcmp(cfg.llm_provider, "llm-openai") != 0 &&
        cfg.llm_provider[0] != '\0') {
        fprintf(stderr, "error: unknown llm provider '%s' (supported: llm-mock, llm-openai)\n",
                cfg.llm_provider);
        return 1;
    }
    if (iter_str) {
        fprintf(stderr, "warning: --max-iterations is ignored by the reactive loop\n");
    }
    if (timeout_str) {
        fprintf(stderr, "warning: --timeout is ignored by the reactive loop\n");
    }

    aegis_coding_agent_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.project_root        = ".";
    acfg.model               = model_name;
    acfg.provider            = cfg.llm_provider;
    acfg.api_key             = api_key;
    acfg.base_url            = base_url;
    acfg.tools               = NULL;
    aegis_coding_agent_t* ca = NULL;
    aegis_status_t        rc = aegis_coding_agent_create(&acfg, &ca);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "error: coding agent create failed: %s\n", aegis_status_str(rc));
        return 1;
    }

    // write pidfile for cancel
    ensure_parent_dir(PIDFILE);
    FILE* pf = fopen(PIDFILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", (int)getpid());
        fclose(pf);
    }

    rc                  = aegis_coding_agent_run(ca, cfg.goal);
    aegis_usage_t total = {0};
    aegis_usage_t last  = {0};
    aegis_coding_agent_usage(ca, &last, &total);
    (void)last;
    save_session_best_effort(ca, cfg.checkpoint_path);
    aegis_coding_agent_destroy(ca);
    unlink(PIDFILE);

    if (rc == AEGIS_OK) {
        printf("run ok: tokens=%llu status=%s\n", (unsigned long long)total.total_tokens,
               aegis_status_str(rc));
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
