#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_tools.h"
#include "aegis/coding/mutations.h"
#include "aegis/tool/tool.h"
#include "aegis/common/cancellation/cancellation.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <time.h>

static aegis_mutation_queue_t* g_mq = NULL;

// ── read ────────────────────────────────────────────────────────────────

static aegis_status_t tool_read_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* v = NULL;
    if (!aegis_tool_args_find(args, "path", &v) || !v || v->type != AEGIS_TOOL_VAL_STRING ||
        !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing path");
    }
    const char* path = v->as.str.ptr;
    // Basic path normalization check
    if (strstr(path, "..")) {
        // Allow but warn; real security gate will handle
    }
    // offset/limit optional
    long offset = 0, limit = 0;
    if (aegis_tool_args_find(args, "offset", &v) && v) {
        if (v->type == AEGIS_TOOL_VAL_INT) {
            offset = (long)v->as.i;
        }
    }
    if (aegis_tool_args_find(args, "limit", &v) && v) {
        if (v->type == AEGIS_TOOL_VAL_INT) {
            limit = (long)v->as.i;
        }
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot stat %s: %s", path, strerror(errno));
        aegis_tool_result_set_string(out, buf);
        return AEGIS_OK;
    }
    if (st.st_size > 1024 * 1024) {  // 1MB limit
        aegis_tool_result_set_string(out, "error: file too large");
        return AEGIS_OK;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot open %s", path);
        aegis_tool_result_set_string(out, buf);
        return AEGIS_OK;
    }
    // Binary detection: check for NUL in first 1k
    char   probe[1024];
    size_t pr = fread(probe, 1, sizeof(probe), f);
    for (size_t i = 0; i < pr; i++) {
        if (probe[i] == '\0') {
            fclose(f);
            aegis_tool_result_set_string(out, "error: binary file");
            return AEGIS_OK;
        }
    }
    fseek(f, 0, SEEK_SET);
    if (offset > 0) {
        fseek(f, offset, SEEK_SET);
    }
    size_t to_read = limit > 0 ? (size_t)limit : (size_t)st.st_size;
    char*  buf     = (char*)malloc(to_read + 1);
    if (!buf) {
        fclose(f);
        return AEGIS_ERR_NOMEM;
    }
    size_t n = fread(buf, 1, to_read, f);
    buf[n]   = '\0';
    fclose(f);
    aegis_status_t st2 = aegis_tool_result_set_string(out, buf);
    free(buf);
    return st2;
}

// ── write (atomic) ─────────────────────────────────────────────────────

static aegis_status_t tool_write_execute(void* user, const aegis_tool_args_t* args,
                                         const aegis_cancellation_token_t* token,
                                         aegis_tool_result_t*              out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* v = NULL;
    if (!aegis_tool_args_find(args, "path", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing path");
    }
    const char* path = v->as.str.ptr;
    if (!aegis_tool_args_find(args, "content", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing content");
    }
    const char* content = v->as.str.ptr;

    if (g_mq) {
        aegis_mutation_queue_acquire(g_mq, path);
    }

    // Ensure parent dir
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* slash          = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0]) {
            char cmd[PATH_MAX + 16];
            snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
            (void)system(cmd);  // best effort; real impl would use mkdir -p via mkdir()
        }
    }

    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot write tmp %s", tmp);
        aegis_tool_result_set_string(out, buf);
        return AEGIS_OK;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        aegis_tool_result_set_string(out, "error: rename failed");
        return AEGIS_OK;
    }
    if (g_mq) {
        aegis_mutation_queue_release(g_mq, path);
    }
    aegis_tool_result_set_string(out, "ok");
    return AEGIS_OK;
}

// ── edit ────────────────────────────────────────────────────────────────

static aegis_status_t tool_edit_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* v = NULL;
    if (!aegis_tool_args_find(args, "path", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing path");
    }
    const char* path = v->as.str.ptr;
    if (!aegis_tool_args_find(args, "old_string", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing old_string");
    }
    const char* old_str = v->as.str.ptr;
    if (!aegis_tool_args_find(args, "new_string", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing new_string");
    }
    const char* new_str = v->as.str.ptr;

    if (g_mq) {
        aegis_mutation_queue_acquire(g_mq, path);
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        aegis_tool_result_set_string(out, "error: cannot open file");
        return AEGIS_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* content = (char*)malloc(sz + 1);
    if (!content) {
        fclose(f);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        return AEGIS_ERR_NOMEM;
    }
    fread(content, 1, sz, f);
    content[sz] = '\0';
    fclose(f);

    char* pos = strstr(content, old_str);
    if (!pos) {
        free(content);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        aegis_tool_result_set_string(out, "error: old_string not found");
        return AEGIS_OK;
    }
    // Ensure unique
    if (strstr(pos + strlen(old_str), old_str)) {
        free(content);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        aegis_tool_result_set_string(out, "error: old_string not unique");
        return AEGIS_OK;
    }

    size_t new_len     = strlen(content) - strlen(old_str) + strlen(new_str) + 1;
    char*  new_content = (char*)malloc(new_len);
    if (!new_content) {
        free(content);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        return AEGIS_ERR_NOMEM;
    }
    size_t prefix = pos - content;
    memcpy(new_content, content, prefix);
    memcpy(new_content + prefix, new_str, strlen(new_str));
    strcpy(new_content + prefix + strlen(new_str), pos + strlen(old_str));
    free(content);

    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
    FILE* outf = fopen(tmp, "wb");
    if (!outf) {
        free(new_content);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        return aegis_tool_result_set_string(out, "error: cannot write tmp");
    }
    fwrite(new_content, 1, strlen(new_content), outf);
    fclose(outf);
    free(new_content);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        if (g_mq) {
            aegis_mutation_queue_release(g_mq, path);
        }
        aegis_tool_result_set_string(out, "error: rename failed");
        return AEGIS_OK;
    }
    if (g_mq) {
        aegis_mutation_queue_release(g_mq, path);
    }
    aegis_tool_result_set_string(out, "ok");
    return AEGIS_OK;
}

// ── bash ────────────────────────────────────────────────────────────────

static aegis_status_t tool_bash_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    const aegis_tool_value_t* v = NULL;
    if (!aegis_tool_args_find(args, "command", &v) || !v || !v->as.str.ptr) {
        return aegis_tool_result_set_string(out, "error: missing command");
    }
    const char* cmd        = v->as.str.ptr;
    long        timeout_ms = 30000;  // default 30s
    if (aegis_tool_args_find(args, "timeout", &v) && v && v->type == AEGIS_TOOL_VAL_INT) {
        timeout_ms = (long)v->as.i;
    }

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
        // child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    // parent
    close(pipefd[1]);
    // Set non-blocking
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    char   buf[8192];
    size_t total  = 0;
    char*  output = NULL;
    size_t cap    = 0;

    struct pollfd pfd       = {.fd = pipefd[0], .events = POLLIN};
    long          elapsed   = 0;
    int           status    = 0;
    bool          timed_out = false;
    while (1) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            kill(pid, SIGTERM);
            timed_out = true;
            break;
        }
        int pr = poll(&pfd, 1, 100);  // 100ms
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                if (total + n + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 4096;
                    while (ncap < total + n + 1) {
                        ncap *= 2;
                    }
                    char* nout = (char*)realloc(output, ncap);
                    if (!nout) {
                        free(output);
                        close(pipefd[0]);
                        kill(pid, SIGKILL);
                        waitpid(pid, NULL, 0);
                        return AEGIS_ERR_NOMEM;
                    }
                    output = nout;
                    cap    = ncap;
                }
                memcpy(output + total, buf, n);
                total += n;
                output[total] = '\0';
            }
        }
        int w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        elapsed += 100;
        if (elapsed >= timeout_ms) {
            kill(pid, SIGTERM);
            // give 500ms to exit gracefully
            for (int i = 0; i < 5; i++) {
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    break;
                }
            }
            if (waitpid(pid, &status, WNOHANG) != pid) {
                kill(pid, SIGKILL), waitpid(pid, &status, 0);
            }
            timed_out = true;
            break;
        }
    }
    close(pipefd[0]);
    // Drain remaining
    while (1) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        if (total + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            while (ncap < total + n + 1) {
                ncap *= 2;
            }
            char* nout = (char*)realloc(output, ncap);
            if (!nout) {
                free(output);
                return AEGIS_ERR_NOMEM;
            }
            output = nout;
            cap    = ncap;
        }
        memcpy(output + total, buf, n);
        total += n;
        output[total] = '\0';
    }
    if (!output) {
        output = strdup("");
    }
    if (timed_out) {
        char* with_status = NULL;
        {
            size_t needed = snprintf(NULL, 0, "timeout after %ldms\n%s", timeout_ms, output) + 1;
            with_status   = (char*)malloc(needed);
            if (with_status) {
                snprintf(with_status, needed, "timeout after %ldms\n%s", timeout_ms, output);
            }
        }
        free(output);
        output = with_status;
        aegis_tool_result_set_string(out, output ? output : "timeout");
        free(output);
        return AEGIS_ERR_TIMEOUT;
    }
    // Append exit code
    char* with_code = NULL;
    int   code      = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    {
        size_t needed = snprintf(NULL, 0, "exit:%d\n%s", code, output) + 1;
        with_code     = (char*)malloc(needed);
        if (with_code) {
            snprintf(with_code, needed, "exit:%d\n%s", code, output);
        }
    }
    free(output);
    aegis_tool_result_set_string(out, with_code ? with_code : "");
    free(with_code);
    return AEGIS_OK;
}

// ── Schema definitions ─────────────────────────────────────────────────

static const aegis_tool_param_spec_t read_params[] = {
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    {.name = "offset", .type = AEGIS_TOOL_VAL_INT, .required = false},
    {.name = "limit", .type = AEGIS_TOOL_VAL_INT, .required = false},
};
static const aegis_tool_schema_t read_schema = {.params = read_params, .param_count = 3};

static const aegis_tool_param_spec_t write_params[] = {
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    {.name = "content", .type = AEGIS_TOOL_VAL_STRING, .required = true},
};
static const aegis_tool_schema_t write_schema = {.params = write_params, .param_count = 2};

static const aegis_tool_param_spec_t edit_params[] = {
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    {.name = "old_string", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    {.name = "new_string", .type = AEGIS_TOOL_VAL_STRING, .required = true},
};
static const aegis_tool_schema_t edit_schema = {.params = edit_params, .param_count = 3};

static const aegis_tool_param_spec_t bash_params[] = {
    {.name = "command", .type = AEGIS_TOOL_VAL_STRING, .required = true},
    {.name = "timeout", .type = AEGIS_TOOL_VAL_INT, .required = false},
};
static const aegis_tool_schema_t bash_schema = {.params = bash_params, .param_count = 2};

const aegis_tool_def_t aegis_coding_tool_read = {
    .name         = "read",
    .description  = "Read file with offset/limit, binary detection, size limit",
    .schema       = read_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_read_execute,
};

const aegis_tool_def_t aegis_coding_tool_write = {
    .name         = "write",
    .description  = "Atomic write with parent dir creation",
    .schema       = write_schema,
    .capabilities = AEGIS_CAP_WRITE_FILE,
    .execute      = tool_write_execute,
};

const aegis_tool_def_t aegis_coding_tool_edit = {
    .name         = "edit",
    .description  = "Edit file with exact unique match",
    .schema       = edit_schema,
    .capabilities = AEGIS_CAP_WRITE_FILE,
    .execute      = tool_edit_execute,
};

const aegis_tool_def_t aegis_coding_tool_bash = {
    .name         = "bash",
    .description  = "Execute shell command via fork/exec",
    .schema       = bash_schema,
    .capabilities = AEGIS_CAP_SHELL | AEGIS_CAP_RUN_PROCESS,
    .execute      = tool_bash_execute,
};

aegis_status_t aegis_coding_tools_register_all(aegis_tool_registry_t*  reg,
                                               aegis_mutation_queue_t* mq)
{
    if (!reg) {
        return AEGIS_ERR_INVALID;
    }
    g_mq = mq;
    aegis_status_t st;
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_read);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_write);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_edit);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_bash);
    if (st != AEGIS_OK) {
        return st;
    }
    return AEGIS_OK;
}
