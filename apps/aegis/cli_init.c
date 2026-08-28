/**
 * @file cli_init.c
 * @brief aegis init command implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

int cmd_init(int argc, char** argv)
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
