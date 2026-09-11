/**
 * @file git_tools.c
 * @brief Git tools (status/diff/commit/log/branch) executed via
 * fork/exec with piped output capture, timeout kill and cooperative
 * cancellation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/git_tools.h"
#include "path_safety.h"
#include "aegis/common/cancellation/cancellation.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Limits ────────────────────────────────────────────────────────────── */

#define GIT_MAX_OUTPUT        (128 * 1024) /* 128KB — diffs can be large */
#define GIT_MAX_LOG           (64 * 1024)  /* 64KB — log/status          */
#define GIT_COMMIT_MSG_MAX    512
#define GIT_LOG_COUNT_MAX     50
#define GIT_LOG_COUNT_DEFAULT 10
#define GIT_PATH_MAX          1024

/* ── Output buffer (same pattern as discovery_tools.c) ────────────────── */

typedef struct git_out_buf {
    char*  buf;
    size_t len;
    size_t cap;
    bool   truncated;
    bool   stopped;
} git_out_buf_t;

static void git_out_init(git_out_buf_t* b)
{
    b->buf       = NULL;
    b->len       = 0;
    b->cap       = 0;
    b->truncated = false;
    b->stopped   = false;
}

static void git_out_destroy(git_out_buf_t* b)
{
    free(b->buf);
    git_out_init(b);
}

static void git_out_append(git_out_buf_t* b, const char* data, size_t n)
{
    if (b->stopped) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + n + 1) {
            ncap *= 2;
        }
        char* nb = (char*)realloc(b->buf, ncap);
        if (!nb) {
            b->stopped = true;
            return;
        }
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void git_out_append_str(git_out_buf_t* b, const char* s)
{
    git_out_append(b, s, strlen(s));
}

/* ── Input validation ──────────────────────────────────────────────────── */

/**
 * @brief Check a string for shell metacharacters.
 *
 * Rejects: $ ` " ' \ ( ) & ; | # ! { } < > \n \r
 * Used to sanitize user-provided commit messages and branch names.
 */
static bool git_arg_is_safe(const char* s)
{
    if (!s) {
        return false;
    }
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '$' || c == '`' || c == '"' || c == '\'' || c == '\\' || c == '(' || c == ')' ||
            c == '&' || c == ';' || c == '|' || c == '#' || c == '!' || c == '{' || c == '}' ||
            c == '<' || c == '>' || c == '\n' || c == '\r') {
            return false;
        }
    }
    return true;
}

/**
 * @brief Validate a git branch name.
 *
 * Rejected characters: anything outside [a-zA-Z0-9._/\-]. Git itself is
 * generous about naming; this is a conservative gate so the executor can
 * pass the name directly to `git branch` without shell-escaping.
 */
static bool git_branch_name_valid(const char* name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '/')) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Return the project root, taken from getcwd().
 *
 * Returns "." on getcwd failure; the caller should handle ENOENT or
 * similar conditions upstream. Static buffer for thread-safety when
 * called from a single-threaded tool executor path (per-turn token).
 */
static const char* git_project_root(void)
{
    static char root[PATH_MAX];
    if (getcwd(root, sizeof(root))) {
        return root;
    }
    return ".";
}

/* ── Git execution helper ──────────────────────────────────────────────── */

/**
 * @brief Execute a git command and capture stdout+stderr.
 *
 * Uses fork/execvp (NOT system()) for security and cancellation support.
 *
 * @param project_root  Working directory for git.
 * @param argv          NULL-terminated argument array (argv[0] = "git").
 * @param token         Cancellation token (polls during wait).
 * @param max_output    Maximum output bytes to capture.
 * @param[out] out      Heap-allocated output string (caller frees).
 * @param[out] exit_code Git exit code.
 * @return AEGIS_OK on success, AEGIS_ERR_CANCELLED if token tripped,
 *         AEGIS_ERR_NOMEM on allocation failure.
 */
static aegis_status_t git_exec(const char* project_root, char** argv,
                               const aegis_cancellation_token_t* token, size_t max_output,
                               char** out, int* exit_code)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return AEGIS_ERR_INTERNAL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return AEGIS_ERR_INTERNAL;
    }

    if (pid == 0) {
        /* child */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (project_root) {
            chdir(project_root);
        }
        execvp("git", argv);
        _exit(127);
    }

    /* parent */
    setpgid(pid, pid);
    close(pipefd[1]);

    /* Non-blocking reads */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    char   buf[8192];
    size_t total  = 0;
    char*  output = NULL;
    size_t cap    = 0;

    struct pollfd pfd     = {.fd = pipefd[0], .events = POLLIN};
    long          elapsed = 0;
    int           status  = 0;

    for (;;) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            kill(-pid, SIGTERM);
            free(output);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            *out       = strdup("");
            *exit_code = -1;
            return AEGIS_ERR_CANCELLED;
        }

        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                if (total + (size_t)n + 1 > max_output) {
                    /* Cap output to prevent runaway */
                    break;
                }
                if (total + (size_t)n + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 4096;
                    while (ncap < total + (size_t)n + 1) {
                        ncap *= 2;
                    }
                    char* nout = (char*)realloc(output, ncap);
                    if (!nout) {
                        free(output);
                        close(pipefd[0]);
                        kill(-pid, SIGKILL);
                        waitpid(pid, NULL, 0);
                        return AEGIS_ERR_NOMEM;
                    }
                    output = nout;
                    cap    = ncap;
                }
                memcpy(output + total, buf, (size_t)n);
                total += (size_t)n;
                output[total] = '\0';
            }
        }

        int w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }

        elapsed += 100;
        if (elapsed >= 30000) {
            /* 30s timeout */
            kill(-pid, SIGTERM);
            for (int i = 0; i < 5; i++) {
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    break;
                }
            }
            if (waitpid(pid, &status, WNOHANG) != pid) {
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            break;
        }
    }

    /* Drain remaining */
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        if (total + (size_t)n + 1 > max_output) {
            break;
        }
        if (total + (size_t)n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            while (ncap < total + (size_t)n + 1) {
                ncap *= 2;
            }
            char* nout = (char*)realloc(output, ncap);
            if (!nout) {
                free(output);
                close(pipefd[0]);
                return AEGIS_ERR_NOMEM;
            }
            output = nout;
            cap    = ncap;
        }
        memcpy(output + total, buf, (size_t)n);
        total += (size_t)n;
        output[total] = '\0';
    }

    close(pipefd[0]);

    if (!output) {
        output = strdup("");
    }

    *out       = output;
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return AEGIS_OK;
}

/* ── git_status ────────────────────────────────────────────────────────── */

/**
 * @brief Execute the "git_status" tool: run `git status --porcelain=v1
 *        --branch` under the project root.
 *
 * Shows working-tree and index state for the optional @p path (default
 * project root). Empty result is folded to "(no changes)". Cancellation
 * propagates as AEGIS_ERR_CANCELLED; all other failures surface as a text
 * message with the git exit code. Output is capped at GIT_MAX_LOG.
 */
static aegis_status_t tool_git_status_execute(void* user, const aegis_tool_args_t* args,
                                              const aegis_cancellation_token_t* token,
                                              aegis_tool_result_t*              out)
{
    (void)user;
    const char*               path = ".";
    const aegis_tool_value_t* v    = NULL;

    if (args && aegis_tool_args_find(args, "path", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        path = v->as.str.ptr;
    }

    if (!aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }

    const char* root = git_project_root();

    char* argv[]      = {"git", "status", "--porcelain=v1", "--branch", "--", (char*)path, NULL};
    char* output      = NULL;
    int   ec          = 0;
    aegis_status_t st = git_exec(root, argv, token, GIT_MAX_LOG, &output, &ec);

    if (st != AEGIS_OK) {
        if (st == AEGIS_ERR_CANCELLED) {
            return st;
        }
        /* git not found or other error — return a helpful message */
        char buf[256];
        snprintf(buf, sizeof(buf), "error: git status failed (exit %d)", ec);
        return aegis_tool_result_set_string(out, buf);
    }

    if (!output || output[0] == '\0') {
        aegis_tool_result_set_string(out, "(no changes)");
        free(output);
        return AEGIS_OK;
    }

    /* Trim trailing newlines for clean output */
    size_t len = strlen(output);
    while (len > 0 && output[len - 1] == '\n') {
        output[--len] = '\0';
    }

    st = aegis_tool_result_set_string(out, output);
    free(output);
    return st;
}

/* ── git_diff ──────────────────────────────────────────────────────────── */

/**
 * @brief Execute the "git_diff" tool: either a symmetric diff against
 * `base` (two-dot), a staged diff (`--staged`), or an unstaged diff of
 * a single path.
 *
 * The three mutually-supported variants are:
 *   - base=X, path= optional  → `git diff X -- path`
 *   - staged=true             → `git diff --staged [-- -- path]`
 *   - path=Y only             → `git diff -- path`
 * @p base is validated for shell-safety; @p path must stay inside the
 * project. Output capped at GIT_MAX_OUTPUT.
 */
static aegis_status_t tool_git_diff_execute(void* user, const aegis_tool_args_t* args,
                                            const aegis_cancellation_token_t* token,
                                            aegis_tool_result_t*              out)
{
    (void)user;
    const char*               path   = NULL;
    bool                      staged = false;
    const char*               base   = NULL;
    const aegis_tool_value_t* v      = NULL;

    if (args && aegis_tool_args_find(args, "path", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        path = v->as.str.ptr;
    }
    if (args && aegis_tool_args_find(args, "staged", &v) && v && v->type == AEGIS_TOOL_VAL_BOOL) {
        staged = v->as.b;
    }
    if (args && aegis_tool_args_find(args, "base", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        base = v->as.str.ptr;
    }

    if (path && !aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }
    if (base && !git_arg_is_safe(base)) {
        return aegis_tool_result_set_string(out, "error: unsafe ref");
    }

    const char* root = git_project_root();

    /* Build argv: up to 8 elements */
    char* argv[8];
    int   argc   = 0;
    argv[argc++] = "git";
    argv[argc++] = "diff";

    if (base) {
        argv[argc++] = (char*)base;
    } else if (staged) {
        argv[argc++] = "--staged";
    }

    if (path) {
        argv[argc++] = "--";
        argv[argc++] = (char*)path;
    }

    argv[argc] = NULL;

    char*          output = NULL;
    int            ec     = 0;
    aegis_status_t st     = git_exec(root, argv, token, GIT_MAX_OUTPUT, &output, &ec);

    if (st != AEGIS_OK) {
        if (st == AEGIS_ERR_CANCELLED) {
            return st;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "error: git diff failed (exit %d)", ec);
        return aegis_tool_result_set_string(out, buf);
    }

    if (!output || output[0] == '\0') {
        aegis_tool_result_set_string(out, "(no changes)");
        free(output);
        return AEGIS_OK;
    }

    st = aegis_tool_result_set_string(out, output);
    free(output);
    return st;
}

/* ── git_commit ────────────────────────────────────────────────────────── */

/**
 * @brief Execute the "git_commit" tool: stage @p files (optional) then
 *        commit with @p message.
 *
 * The message must be non-empty, <= 512 chars and pass git_arg_is_safe()
 * (no shell metacharacters). When @p files is given it's a comma-separated
 * list of project-relative paths; each is validated via aegis_safe_relative_path
 * and the whole list is handed to `git add --` before `git commit`. Returns
 * "(no changes to commit)" when git exits 0 with empty diff output.
 */
static aegis_status_t tool_git_commit_execute(void* user, const aegis_tool_args_t* args,
                                              const aegis_cancellation_token_t* token,
                                              aegis_tool_result_t*              out)
{
    (void)user;
    const char*               message = NULL;
    const char*               files   = NULL;
    const aegis_tool_value_t* v       = NULL;

    if (args && aegis_tool_args_find(args, "message", &v) && v &&
        v->type == AEGIS_TOOL_VAL_STRING && v->as.str.ptr) {
        message = v->as.str.ptr;
    }
    if (!message || message[0] == '\0') {
        return aegis_tool_result_set_string(out, "error: missing message");
    }
    if (strlen(message) > GIT_COMMIT_MSG_MAX) {
        return aegis_tool_result_set_string(out, "error: commit message too long (max 512 chars)");
    }
    if (!git_arg_is_safe(message)) {
        return aegis_tool_result_set_string(out, "error: unsafe commit message");
    }

    if (args && aegis_tool_args_find(args, "files", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        files = v->as.str.ptr;
    }

    const char* root = git_project_root();

    /* Step 1: Stage files (if specified) */
    if (files && files[0] != '\0') {
        /* Split comma-separated file list */
        char files_copy[GIT_PATH_MAX];
        snprintf(files_copy, sizeof(files_copy), "%s", files);

        /* Build git add argv: ["git", "add", "--", file1, file2, ..., NULL]
         * Max 32 files */
        char* add_argv[34];
        int   add_argc       = 0;
        add_argv[add_argc++] = "git";
        add_argv[add_argc++] = "add";
        add_argv[add_argc++] = "--";

        char* saveptr = NULL;
        char* token_r = strtok_r(files_copy, ",", &saveptr);
        while (token_r && add_argc < 33) {
            /* Trim leading/trailing whitespace */
            while (*token_r == ' ' || *token_r == '\t') {
                token_r++;
            }
            char* end = token_r + strlen(token_r) - 1;
            while (end > token_r && (*end == ' ' || *end == '\t')) {
                *end-- = '\0';
            }

            if (!aegis_safe_relative_path(token_r)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "error: unsafe file path: %s", token_r);
                return aegis_tool_result_set_string(out, buf);
            }
            add_argv[add_argc++] = token_r;
            token_r              = strtok_r(NULL, ",", &saveptr);
        }
        add_argv[add_argc] = NULL;

        char*          add_output = NULL;
        int            add_ec     = 0;
        aegis_status_t ast        = git_exec(root, add_argv, token, 4096, &add_output, &add_ec);
        free(add_output);

        if (ast != AEGIS_OK) {
            return ast;
        }
    } else {
        /* Stage all changes (tracked + untracked) */
        char*          add_argv[] = {"git", "add", "-A", NULL};
        char*          add_output = NULL;
        int            add_ec     = 0;
        aegis_status_t ast        = git_exec(root, add_argv, token, 4096, &add_output, &add_ec);
        free(add_output);

        if (ast != AEGIS_OK) {
            return ast;
        }
    }

    /* Step 2: Commit */
    char*          commit_argv[] = {"git", "commit", "-m", (char*)message, NULL};
    char*          commit_output = NULL;
    int            commit_ec     = 0;
    aegis_status_t cst = git_exec(root, commit_argv, token, 4096, &commit_output, &commit_ec);

    if (cst != AEGIS_OK) {
        if (cst == AEGIS_ERR_CANCELLED) {
            return cst;
        }
        return aegis_tool_result_set_string(out, "error: git commit failed");
    }

    /* Parse commit hash from output */
    char result[512] = {0};
    if (commit_output && commit_output[0] != '\0') {
        /* Look for "[branch hash] message" pattern or "nothing to commit" */
        const char* hash = strstr(commit_output, "[");
        if (hash) {
            hash = strchr(hash, ' ');
            if (hash) {
                hash++;
                const char* end = strchr(hash, ' ');
                if (end) {
                    size_t hlen = (size_t)(end - hash);
                    if (hlen > 12) {
                        hlen = 12;
                    }
                    snprintf(result, sizeof(result), "Committed %.*s: \"%s\"", (int)hlen, hash,
                             message);
                }
            }
        }
        if (result[0] == '\0') {
            /* Fallback: check for "nothing to commit" */
            if (strstr(commit_output, "nothing to commit")) {
                snprintf(result, sizeof(result), "nothing to commit (working tree clean)");
            } else {
                /* Use first line of output */
                const char* nl  = strchr(commit_output, '\n');
                size_t      len = nl ? (size_t)(nl - commit_output) : strlen(commit_output);
                if (len > sizeof(result) - 32) {
                    len = sizeof(result) - 32;
                }
                snprintf(result, sizeof(result), "%.*s", (int)len, commit_output);
            }
        }
    } else {
        snprintf(result, sizeof(result), "Committed (exit %d)", commit_ec);
    }

    free(commit_output);
    return aegis_tool_result_set_string(out, result);
}

/* ── git_log ───────────────────────────────────────────────────────────── */

/**
 * @brief Execute the "git_log" tool: return the most-recent N commits,
 *        optionally filtered by @p path.
 *
 * Supports three --format modes: "oneline" (default, %h %s), "short"
 * (%h %ad %an: %s with --date=short) and "full" (%H + author + %s).
 * @p count is clamped to [1, GIT_LOG_COUNT_MAX]. Output capped at
 * GIT_MAX_LOG. Path must stay inside the project.
 */
static aegis_status_t tool_git_log_execute(void* user, const aegis_tool_args_t* args,
                                           const aegis_cancellation_token_t* token,
                                           aegis_tool_result_t*              out)
{
    (void)user;
    int                       count  = GIT_LOG_COUNT_DEFAULT;
    const char*               path   = NULL;
    const char*               format = "oneline";
    const aegis_tool_value_t* v      = NULL;

    if (args && aegis_tool_args_find(args, "count", &v) && v && v->type == AEGIS_TOOL_VAL_INT) {
        count = (int)v->as.i;
        if (count < 1) {
            count = 1;
        }
        if (count > GIT_LOG_COUNT_MAX) {
            count = GIT_LOG_COUNT_MAX;
        }
    }

    if (args && aegis_tool_args_find(args, "path", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        path = v->as.str.ptr;
    }

    if (args && aegis_tool_args_find(args, "format", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        format = v->as.str.ptr;
    }

    if (path && !aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }

    const char* root = git_project_root();

    /* Build format string as --format=<fmt> */
    char fmt_buf[128];
    if (strcmp(format, "full") == 0) {
        snprintf(fmt_buf, sizeof(fmt_buf),
                 "--format=%%H%%n%%ad%%nAuthor: %%an <%%ae>%%n%%n    %%s%%n");
    } else if (strcmp(format, "short") == 0) {
        snprintf(fmt_buf, sizeof(fmt_buf), "--format=%%h %%ad %%an: %%s");
    } else {
        /* oneline (default) */
        snprintf(fmt_buf, sizeof(fmt_buf), "--format=%%h %%s");
    }

    /* Build argv */
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", count);

    char* argv[8];
    int   argc   = 0;
    argv[argc++] = "git";
    argv[argc++] = "log";
    argv[argc++] = fmt_buf;
    argv[argc++] = "-n";
    argv[argc++] = count_str;

    if (strcmp(format, "full") == 0 || strcmp(format, "short") == 0) {
        argv[argc++] = "--date=short";
    }

    if (path) {
        argv[argc++] = "--";
    }
    argv[argc] = NULL;

    /* If path, we need to append it */
    char* final_argv[10];
    int   fargc = 0;
    for (int i = 0; i < argc; i++) {
        final_argv[fargc++] = argv[i];
    }
    if (path) {
        final_argv[fargc++] = (char*)path;
    }
    final_argv[fargc] = NULL;

    char*          output = NULL;
    int            ec     = 0;
    aegis_status_t st     = git_exec(root, final_argv, token, GIT_MAX_LOG, &output, &ec);

    if (st != AEGIS_OK) {
        if (st == AEGIS_ERR_CANCELLED) {
            return st;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "error: git log failed (exit %d)", ec);
        return aegis_tool_result_set_string(out, buf);
    }

    if (!output || output[0] == '\0') {
        aegis_tool_result_set_string(out, "(no commits)");
        free(output);
        return AEGIS_OK;
    }

    /* Trim trailing newlines */
    size_t len = strlen(output);
    while (len > 0 && output[len - 1] == '\n') {
        output[--len] = '\0';
    }

    st = aegis_tool_result_set_string(out, output);
    free(output);
    return st;
}

/* ── git_branch ────────────────────────────────────────────────────────── */

/**
 * @brief Execute the "git_branch" tool: list/create/delete/switch branches.
 *
 * Three actions are supported:
 *   - "list" → `git branch -a --list` (all refs, capped at GIT_MAX_LOG)
 *   - "create" <name> → `git checkout -b <name>` (name validated by
 *                      git_branch_name_valid before reaching the shell)
 *   - "delete" <name> / "switch" <name> via `git checkout <name>`
 *
 * Cancellation and 30 s timeout are delegated to git_exec(). Empty result
 * is rendered as "(no branches)".
 */
static aegis_status_t tool_git_branch_execute(void* user, const aegis_tool_args_t* args,
                                              const aegis_cancellation_token_t* token,
                                              aegis_tool_result_t*              out)
{
    (void)user;
    const char*               action = "list";
    const char*               name   = NULL;
    const aegis_tool_value_t* v      = NULL;

    if (args && aegis_tool_args_find(args, "action", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        action = v->as.str.ptr;
    }

    if (args && aegis_tool_args_find(args, "name", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        name = v->as.str.ptr;
    }

    const char* root = git_project_root();

    if (strcmp(action, "list") == 0) {
        char*          argv[] = {"git", "branch", "-a", "--list", NULL};
        char*          output = NULL;
        int            ec     = 0;
        aegis_status_t st     = git_exec(root, argv, token, GIT_MAX_LOG, &output, &ec);

        if (st != AEGIS_OK) {
            if (st == AEGIS_ERR_CANCELLED) {
                return st;
            }
            return aegis_tool_result_set_string(out, "error: git branch list failed");
        }

        if (!output || output[0] == '\0') {
            aegis_tool_result_set_string(out, "(no branches)");
            free(output);
            return AEGIS_OK;
        }

        size_t len = strlen(output);
        while (len > 0 && output[len - 1] == '\n') {
            output[--len] = '\0';
        }

        st = aegis_tool_result_set_string(out, output);
        free(output);
        return st;
    }

    if (strcmp(action, "create") == 0) {
        if (!name || name[0] == '\0') {
            return aegis_tool_result_set_string(out, "error: missing branch name");
        }
        if (!git_branch_name_valid(name)) {
            return aegis_tool_result_set_string(out, "error: invalid branch name");
        }

        char*          argv[] = {"git", "branch", (char*)name, NULL};
        char*          output = NULL;
        int            ec     = 0;
        aegis_status_t st     = git_exec(root, argv, token, 4096, &output, &ec);

        if (st != AEGIS_OK) {
            if (st == AEGIS_ERR_CANCELLED) {
                return st;
            }
            return aegis_tool_result_set_string(out, "error: git branch create failed");
        }

        char result[256];
        if (ec == 0) {
            snprintf(result, sizeof(result), "Created branch %s", name);
        } else {
            snprintf(result, sizeof(result), "error: branch creation failed (exit %d)%s", ec,
                     output ? ": " : "");
            if (output && output[0]) {
                /* Append git's error message */
                size_t rlen = strlen(result);
                size_t olen = strlen(output);
                if (rlen + olen < sizeof(result) - 1) {
                    memcpy(result + rlen, output, olen + 1);
                }
            }
        }
        free(output);
        return aegis_tool_result_set_string(out, result);
    }

    if (strcmp(action, "switch") == 0) {
        if (!name || name[0] == '\0') {
            return aegis_tool_result_set_string(out, "error: missing branch name");
        }
        if (!git_branch_name_valid(name)) {
            return aegis_tool_result_set_string(out, "error: invalid branch name");
        }

        char*          argv[] = {"git", "switch", (char*)name, NULL};
        char*          output = NULL;
        int            ec     = 0;
        aegis_status_t st     = git_exec(root, argv, token, 4096, &output, &ec);

        if (st != AEGIS_OK) {
            if (st == AEGIS_ERR_CANCELLED) {
                return st;
            }
            return aegis_tool_result_set_string(out, "error: git switch failed");
        }

        char result[256];
        if (ec == 0) {
            snprintf(result, sizeof(result), "Switched to branch %s", name);
        } else {
            snprintf(result, sizeof(result), "error: switch failed (exit %d)", ec);
        }
        free(output);
        return aegis_tool_result_set_string(out, result);
    }

    return aegis_tool_result_set_string(out, "error: unknown action (use list, create, or switch)");
}

/* ── Schema definitions ───────────────────────────────────────────────── */

/* git_status */
static const aegis_tool_param_spec_t git_status_params[] = {
    {.name        = "path",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Subdirectory to check (default: project root)"},
};
static const aegis_tool_schema_t git_status_schema = {.params      = git_status_params,
                                                      .param_count = 1};

/* git_diff */
static const aegis_tool_param_spec_t git_diff_params[] = {
    {.name        = "path",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "File or directory to diff (default: all)"},
    {.name        = "staged",
     .type        = AEGIS_TOOL_VAL_BOOL,
     .required    = false,
     .description = "Show staged changes instead of working tree (default: false)"},
    {.name        = "base",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Base commit/ref for comparison (default: HEAD)"},
};
static const aegis_tool_schema_t git_diff_schema = {.params = git_diff_params, .param_count = 3};

/* git_commit */
static const aegis_tool_param_spec_t git_commit_params[] = {
    {.name        = "message",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = true,
     .description = "Commit message (max 512 chars, no shell metacharacters)"},
    {.name        = "files",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Comma-separated file paths to stage (default: all tracked changes)"},
};
static const aegis_tool_schema_t git_commit_schema = {.params      = git_commit_params,
                                                      .param_count = 2};

/* git_log */
static const aegis_tool_param_spec_t git_log_params[] = {
    {.name        = "count",
     .type        = AEGIS_TOOL_VAL_INT,
     .required    = false,
     .description = "Number of commits (default: 10, max: 50)"},
    {.name        = "path",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Filter by file or directory"},
    {.name        = "format",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Output format: oneline (default), short, full"},
};
static const aegis_tool_schema_t git_log_schema = {.params = git_log_params, .param_count = 3};

/* git_branch */
static const aegis_tool_param_spec_t git_branch_params[] = {
    {.name        = "action",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = true,
     .description = "Action: list, create, or switch"},
    {.name        = "name",
     .type        = AEGIS_TOOL_VAL_STRING,
     .required    = false,
     .description = "Branch name (required for create/switch)"},
};
static const aegis_tool_schema_t git_branch_schema = {.params      = git_branch_params,
                                                      .param_count = 2};

/* ── Tool definitions ──────────────────────────────────────────────────── */

const aegis_tool_def_t aegis_coding_tool_git_status = {
    .name         = "git_status",
    .description  = "Show working tree status: branch, modified/added/deleted files",
    .schema       = git_status_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_git_status_execute,
};

const aegis_tool_def_t aegis_coding_tool_git_diff = {
    .name         = "git_diff",
    .description  = "Show unified diff of changes (working tree, staged, or between commits)",
    .schema       = git_diff_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_git_diff_execute,
};

const aegis_tool_def_t aegis_coding_tool_git_commit = {
    .name         = "git_commit",
    .description  = "Stage files and create a git commit",
    .schema       = git_commit_schema,
    .capabilities = AEGIS_CAP_SHELL | AEGIS_CAP_WRITE_FILE,
    .execute      = tool_git_commit_execute,
};

const aegis_tool_def_t aegis_coding_tool_git_log = {
    .name         = "git_log",
    .description  = "Show recent commit history with stats",
    .schema       = git_log_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_git_log_execute,
};

const aegis_tool_def_t aegis_coding_tool_git_branch = {
    .name         = "git_branch",
    .description  = "List, create, or switch git branches",
    .schema       = git_branch_schema,
    .capabilities = AEGIS_CAP_SHELL | AEGIS_CAP_READ_FILE,
    .execute      = tool_git_branch_execute,
};

/* ── Registration ──────────────────────────────────────────────────────── */

/**
 * @brief Register the five git tools (status, diff, commit, log, branch).
 *
 * All git commands run via fork/exec under the project root (getcwd()) with
 * a hard 128 KB / 64 KB output cap depending on the command, and a
 * 30-second timeout. Branch-name and commit-message inputs are validated
 * against a safe-character whitelist before reaching the shell.
 *
 * @param reg Registry to populate (must be non-NULL).
 * @return AEGIS_OK when all five tools registered, else the first error.
 */
aegis_status_t aegis_coding_git_tools_register_all(aegis_tool_registry_t* reg)
{
    if (!reg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st;
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_git_status);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_git_diff);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_git_commit);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_git_log);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_git_branch);
    if (st != AEGIS_OK) {
        return st;
    }
    return AEGIS_OK;
}
