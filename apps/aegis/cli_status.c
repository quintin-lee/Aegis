/**
 * @file cli_status.c
 * @brief aegis status command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include "aegis/session/session.h"

int cmd_status(int argc, char** argv)
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
    } else if (rc != AEGIS_OK) {
        fprintf(stderr, "error: checkpoint corrupted at '%s': %s (%s)\n", ckpt_path,
                aegis_status_str(rc), aegis_checkpoint_status_str(st));
        if (ckpt) {
            aegis_checkpoint_destroy(ckpt);
        }
        return 1;
    } else {
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
    }
    {
        char sess_path[1024];
        cli_session_path_for_checkpoint(ckpt_path, sess_path, sizeof(sess_path));
        if (access(sess_path, F_OK) == 0) {
            aegis_session_t* sess = NULL;
            aegis_status_t   src  = aegis_session_load(sess_path, &sess);
            if (src != AEGIS_OK || !sess) {
                fprintf(stderr, "error: session corrupted at '%s': %s\n", sess_path,
                        aegis_status_str(src));
                return 1;
            }
            printf("session: %s\n", sess_path);
            printf("messages: %zu\n", aegis_session_message_count(sess));
            aegis_session_destroy(sess);
        }
    }
    return 0;
}

/* ── cancel ────────────────────────────────────────────────────────────────── */
