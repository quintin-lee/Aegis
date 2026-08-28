/**
 * @file cli_main.c
 * @brief Aegis CLI: main entry point and command dispatch.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/cli_helpers.h"

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
