/**
 * @file main.c
 * @brief Aegis CLI — interactive, print, resume, and legacy commands.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"
#include <string.h>
#include <stdio.h>

// New interactive/print handlers
int cmd_interactive(const char* project_root, const char* model, const char* resume_path);
int cmd_print(const char* prompt, const char* project_root, const char* model);

int main(int argc, char** argv)
{
    // No args → interactive (Pi-like)
    if (argc == 1) {
        return cmd_interactive(".", NULL, NULL);
    }

    // Parse global flags before subcommand
    const char* model        = NULL;
    const char* cwd          = NULL;
    const char* resume_path  = NULL;
    const char* print_prompt = NULL;
    int         is_print     = 0;
    int         is_resume    = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strncmp(argv[i], "--model=", 8) == 0) {
            model = argv[i] + 8;
        } else if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            cwd = argv[++i];
        } else if (strncmp(argv[i], "--cwd=", 6) == 0) {
            cwd = argv[i] + 6;
        } else if (strcmp(argv[i], "--resume") == 0) {
            is_resume = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                resume_path = argv[++i];
            }
        } else if (strncmp(argv[i], "--resume=", 9) == 0) {
            is_resume   = 1;
            resume_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--print") == 0) {
            is_print = 1;
            if (i + 1 < argc) {
                print_prompt = argv[++i];
            } else {
                print_prompt = "";
            }
        } else if (strncmp(argv[i], "--print=", 8) == 0) {
            is_print     = 1;
            print_prompt = argv[i] + 8;
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            resume_path = argv[++i];
            is_resume   = 1;
        }
        // legacy commands handled below
    }

    if (is_print) {
        const char* prompt = print_prompt ? print_prompt : "";
        // If print prompt is empty and there are remaining args as prompt
        if (prompt[0] == '\0') {
            // collect remaining non-flag args as prompt
            static char buf[8192];
            buf[0] = '\0';
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] != '-' || strcmp(argv[i], "--print") == 0) {
                    continue;
                }
                if (strlen(buf) + strlen(argv[i]) + 2 < sizeof(buf)) {
                    if (buf[0]) {
                        strcat(buf, " ");
                    }
                    strcat(buf, argv[i]);
                }
            }
            if (buf[0]) {
                prompt = buf;
            }
        }
        return cmd_print(prompt, cwd, model);
    }

    if (argc == 2 && is_resume) {
        // aegis --resume
        return cmd_interactive(cwd ? cwd : ".", model, resume_path);
    }

    // Legacy subcommands
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
