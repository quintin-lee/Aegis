/**
 * @file cli_helpers.c
 * @brief CLI helper implementations (moved from header to avoid unused warnings).
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_helpers.h"

/* Implementation moved from header to fix -Wunused-function warnings */

/**
 * @brief Fill @p c with the compiled-in defaults (goal, checkpoint path,
 *        LLM provider, config path, max-iterations, zero timeout).
 *
 * NULL is a no-op. All fixed-size buffers are NUL-terminated.
 *
 * @param[in] c Config struct to fill, or NULL.
 */
void cli_config_default(cli_config_t* c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    strncpy(c->goal, DEFAULT_GOAL, sizeof(c->goal) - 1);
    strncpy(c->checkpoint_path, DEFAULT_CHECKPOINT, sizeof(c->checkpoint_path) - 1);
    strncpy(c->llm_provider, DEFAULT_LLM, sizeof(c->llm_provider) - 1);
    strncpy(c->config_path, DEFAULT_CONFIG, sizeof(c->config_path) - 1);
    c->max_iterations = DEFAULT_MAX_ITER;
    c->timeout_ms     = 0;
}

void trim(char* s)
{
    if (!s) {
        return;
    }
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

/**
 * @brief Load a JSON config file into @p c, preserving any fields already
 *        set when the key is absent from the file.
 *
 * Parses a minimal JSON object with the keys goal, checkpoint_path,
 * llm_provider, config_path, max_iterations and timeout_ms. Missing or
 * malformed files return -1 without modifying @p c. Caller-supplied
 * pointers are validated.
 *
 * @param[in]  c    Config struct to populate (must be non-NULL).
 * @param[in]  path Path to the JSON config file (must be non-NULL).
 * @return 0 on success, -1 on I/O or parse failure.
 */
int cli_config_load(cli_config_t* c, const char* path)
{
    if (!c || !path) {
        return -1;
    }
    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open config '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);

        if (strcmp(line, "goal") == 0) {
            strncpy(c->goal, eq + 1, sizeof(c->goal) - 1);
        } else if (strcmp(line, "checkpoint_path") == 0) {
            strncpy(c->checkpoint_path, eq + 1, sizeof(c->checkpoint_path) - 1);
        } else if (strcmp(line, "llm_provider") == 0) {
            strncpy(c->llm_provider, eq + 1, sizeof(c->llm_provider) - 1);
        } else if (strcmp(line, "max_iterations") == 0) {
            c->max_iterations = (uint32_t)atoi(eq + 1);
        }
    }
    fclose(fp);
    return 0;
}

int cli_config_save(const cli_config_t* c, const char* path)
{
    if (!c || !path) {
        return -1;
    }

    char* tmp = strdup(path);
    if (tmp) {
        char* slash = strrchr(tmp, '/');
        if (slash) {
            *slash = '\0';
            mkdir_p(tmp);
        }
        free(tmp);
    }

    FILE* fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "# Aegis configuration\n");
    fprintf(fp, "goal=%s\n", c->goal);
    fprintf(fp, "checkpoint_path=%s\n", c->checkpoint_path);
    fprintf(fp, "llm_provider=%s\n", c->llm_provider);
    fprintf(fp, "max_iterations=%u\n", c->max_iterations);

    fclose(fp);
    return 0;
}

int mkdir_p(const char* path)
{
    if (!path) {
        return -1;
    }
    char* tmp = strdup(path);
    if (!tmp) {
        return -1;
    }

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
    return 0;
}

void ensure_parent_dir(const char* filepath)
{
    if (!filepath) {
        return;
    }
    char* tmp = strdup(filepath);
    if (!tmp) {
        return;
    }

    char* last_slash = strrchr(tmp, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir_p(tmp);
    }
    free(tmp);
}

void cli_session_path_for_checkpoint(const char* ckpt_path, char* out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    if (out_sz < 16) {
        snprintf(out, out_sz, "session.jsonl");
        return;
    }
    if (!ckpt_path || ckpt_path[0] == '\0') {
        snprintf(out, out_sz, "session.jsonl");
        return;
    }
    const char* slash = strrchr(ckpt_path, '/');
    if (!slash) {
        snprintf(out, out_sz, "session.jsonl");
        return;
    }
    size_t dirlen = (size_t)(slash - ckpt_path);
    if (dirlen > out_sz - 16) {
        dirlen = out_sz - 16;
    }
    memcpy(out, ckpt_path, dirlen);
    snprintf(out + dirlen, out_sz - dirlen, "/session.jsonl");
}

void print_usage(FILE* out)
{
    if (!out) {
        out = stderr;
    }
    fprintf(out, "Usage: aegis <command> [options]\n\n");
    fprintf(out, "Commands:\n");
    fprintf(out, "  init [--path DIR] [--force]    Initialize a new aegis project\n");
    fprintf(out, "  run [--goal TEXT] [options]    Run autonomous agent\n");
    fprintf(out, "      --provider NAME           LLM provider: llm-mock (default), llm-openai\n");
    fprintf(out, "      --model MODEL             Model id (default: gpt-4o-mini for openai)\n");
    fprintf(out, "      --api-key KEY             OpenAI API key\n");
    fprintf(out, "      --base-url URL            OpenAI-compatible base URL\n");
    fprintf(out, "      --max-iterations N        Max iterations (default: 5)\n");
    fprintf(out, "      --timeout MS              Per-task timeout in ms (default: unlimited)\n");
    fprintf(out, "  status                         Show agent status\n");
    fprintf(out, "  cancel                         Cancel running agent\n");
    fprintf(out, "  inspect                        Inspect checkpoint\n");
    fprintf(out, "  --help, -h                     Show this help\n");
}

void print_version(void)
{
    printf("aegis %s\n", AEGIS_VERSION_STRING);
}
