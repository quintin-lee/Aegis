/**
 * @file cli_inspect.c
 * @brief aegis inspect command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

int cmd_inspect(int argc, char** argv)
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
