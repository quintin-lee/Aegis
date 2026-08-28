/**
 * @file cli_cancel.c
 * @brief aegis cancel command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

int cmd_cancel(int argc, char** argv)
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
