#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/discovery_tools.h"
#include "path_safety.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ── Limits ──────────────────────────────────────────────────────────────

#define DISC_MAX_RESULTS 200
#define DISC_MAX_BYTES   (64 * 1024)
#define DISC_MAX_DEPTH   32
#define DISC_MAX_LINE    4096
#define DISC_PATH_MAX    1024

// ── Output buffer ───────────────────────────────────────────────────────

typedef struct out_buf {
    char*  buf;
    size_t len;
    size_t cap;
    size_t count;    /**< result lines emitted */
    bool   truncated;
    bool   stopped;  /**< set once a cap is hit; walker aborts */
} out_buf_t;

static void out_buf_init(out_buf_t* b)
{
    b->buf       = NULL;
    b->len       = 0;
    b->cap       = 0;
    b->count     = 0;
    b->truncated = false;
    b->stopped   = false;
}

static void out_buf_destroy(out_buf_t* b)
{
    free(b->buf);
    out_buf_init(b);
}

/** Append one line. Silently switches to truncated mode past the caps. */
static void out_buf_append_line(out_buf_t* b, const char* line)
{
    if (b->stopped) {
        return;
    }
    size_t line_len = strlen(line);
    if (b->count >= DISC_MAX_RESULTS || b->len + line_len + 2 > DISC_MAX_BYTES) {
        b->truncated = true;
        b->stopped   = true;
        return;
    }
    if (b->len + line_len + 2 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 1024;
        while (ncap < b->len + line_len + 2) {
            ncap *= 2;
        }
        char* nbuf = (char*)realloc(b->buf, ncap);
        if (!nbuf) {
            b->stopped = true;
            return;
        }
        b->buf = nbuf;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, line, line_len);
    b->len += line_len;
    b->buf[b->len++] = '\n';
    b->buf[b->len]   = '\0';
    b->count++;
}

// ── Recursive walk ──────────────────────────────────────────────────────

typedef aegis_status_t (*visit_fn)(const char* rel, const char* full, void* user, bool* stop);

typedef struct walk_ctx {
    const aegis_cancellation_token_t* token;
    bool                              cancelled;
} walk_ctx_t;

/**
 * Depth-first walk rooted at (root + rel). Skips "." / ".." and never
 * descends into ".git". Calls visit() for every regular file with a
 * project-relative path. Non-zero visit return aborts the walk and is
 * propagated verbatim.
 */
static aegis_status_t walk_tree(const char* root, const char* rel, int depth, visit_fn visit,
                                void* user, walk_ctx_t* wctx)
{
    if (depth > DISC_MAX_DEPTH) {
        return AEGIS_OK;
    }
    if (wctx->token && aegis_cancellation_token_is_cancelled(wctx->token)) {
        wctx->cancelled = true;
        return AEGIS_ERR_CANCELLED;
    }
    if (rel[0] == '\0' && depth > 0) {
        return AEGIS_OK;  // guard: never call with empty rel beyond the root
    }

    char dirpath[DISC_PATH_MAX];
    if (rel[0] == '\0') {
        snprintf(dirpath, sizeof(dirpath), "%s", root);
    } else {
        snprintf(dirpath, sizeof(dirpath), "%s/%s", root, rel);
    }

    DIR* d = opendir(dirpath);
    if (!d) {
        // Caller validated the root; per-directory errors are surfaced by
        // the tool level as inline error strings where meaningful.
        return AEGIS_OK;
    }
    struct dirent* ent;
    aegis_status_t rc = AEGIS_OK;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (wctx->token && aegis_cancellation_token_is_cancelled(wctx->token)) {
            wctx->cancelled = true;
            rc              = AEGIS_ERR_CANCELLED;
            break;
        }
        char child_rel[DISC_PATH_MAX];
        if (rel[0] == '\0') {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
        }
        char child_full[DISC_PATH_MAX];
        snprintf(child_full, sizeof(child_full), "%s/%s", root, child_rel);

        struct stat st;
        if (lstat(child_full, &st) != 0) {
            continue;  // vanished or unreadable: skip silently
        }
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(ent->d_name, ".git") == 0) {
                continue;
            }
            rc = walk_tree(root, child_rel, depth + 1, visit, user, wctx);
            if (rc != AEGIS_OK) {
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            bool stop = false;
            rc        = visit(child_rel, child_full, user, &stop);
            if (rc != AEGIS_OK || stop) {
                break;
            }
        }
        // symlinks/devices/etc: ignored
    }
    closedir(d);
    return rc;
}

// ── list ────────────────────────────────────────────────────────────────

typedef struct sort_entry {
    char name[256];
    bool is_dir;
} sort_entry_t;

static int sort_entry_cmp(const void* a, const void* b)
{
    const sort_entry_t* ea = (const sort_entry_t*)a;
    const sort_entry_t* eb = (const sort_entry_t*)b;
    // Directories first, then lexicographic.
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

static aegis_status_t tool_list_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    const char*           path = ".";
    const aegis_tool_value_t* v  = NULL;
    if (args && aegis_tool_args_find(args, "path", &v) && v &&
        v->type == AEGIS_TOOL_VAL_STRING && v->as.str.ptr) {
        path = v->as.str.ptr;
    }
    if (!aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }

    DIR* d = opendir(path);
    if (!d) {
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot open %s: %s", path, strerror(errno));
        return aegis_tool_result_set_string(out, buf);
    }

    // Collect entries so we can sort (dirs first) before emitting.
    size_t         n       = 0;
    size_t         cap     = 64;
    sort_entry_t* entries = (sort_entry_t*)malloc(cap * sizeof(*entries));
    if (!entries) {
        closedir(d);
        return AEGIS_ERR_NOMEM;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            size_t        ncap     = cap * 2;
            sort_entry_t* nentries = (sort_entry_t*)realloc(entries, ncap * sizeof(*entries));
            if (!nentries) {
                free(entries);
                closedir(d);
                return AEGIS_ERR_NOMEM;
            }
            entries = nentries;
            cap     = ncap;
        }
        char full[DISC_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        snprintf(entries[n].name, sizeof(entries[n].name), "%s", ent->d_name);
        entries[n].is_dir = S_ISDIR(st.st_mode);
        n++;
    }
    qsort(entries, n, sizeof(*entries), sort_entry_cmp);

    out_buf_t ob;
    out_buf_init(&ob);
    for (size_t i = 0; i < n; i++) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            free(entries);
            closedir(d);
            out_buf_destroy(&ob);
            return AEGIS_ERR_CANCELLED;
        }
        char full[DISC_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, entries[i].name);
        struct stat fst;
        if (stat(full, &fst) != 0) {
            continue;
        }
        char line[DISC_PATH_MAX + 64];
        if (entries[i].is_dir) {
            snprintf(line, sizeof(line), "%s\tdir\t%llu", entries[i].name, (unsigned long long)0);
        } else {
            snprintf(line, sizeof(line), "%s\tfile\t%llu", entries[i].name,
                     (unsigned long long)fst.st_size);
        }
        out_buf_append_line(&ob, line);
        if (ob.stopped) {
            break;
        }
    }
    free(entries);
    closedir(d);

    if (ob.buf == NULL) {
        out_buf_destroy(&ob);
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        return aegis_tool_result_set_string(out, "(empty)");
    }
    if (ob.truncated) {
        size_t need = ob.len + 32;
        char*  nbuf = (char*)realloc(ob.buf, need);
        if (nbuf) {
            ob.buf = nbuf;
            memcpy(ob.buf + ob.len, "... truncated\n", 15);
            ob.len += 14;
        }
    }
    aegis_status_t st = aegis_tool_result_set_string(out, ob.buf);
    out_buf_destroy(&ob);
    return st;
}

// ── glob ────────────────────────────────────────────────────────────────

typedef struct glob_ctx {
    out_buf_t  out;
    const char* pattern;
} glob_ctx_t;

static const char* path_basename(const char* p)
{
    const char* slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static aegis_status_t glob_visit(const char* rel, const char* full, void* user, bool* stop)
{
    (void)full;
    glob_ctx_t* gc = (glob_ctx_t*)user;
    if (fnmatch(gc->pattern, rel, 0) == 0 || fnmatch(gc->pattern, path_basename(rel), 0) == 0) {
        out_buf_append_line(&gc->out, rel);
        if (gc->out.stopped) {
            *stop = true;
        }
    }
    return AEGIS_OK;
}

static aegis_status_t tool_glob_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    const aegis_tool_value_t* v = NULL;
    const char*               pattern = NULL;
    const char*               path    = ".";
    if (aegis_tool_args_find(args, "pattern", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        pattern = v->as.str.ptr;
    }
    if (!pattern) {
        return aegis_tool_result_set_string(out, "error: missing pattern");
    }
    if (aegis_tool_args_find(args, "path", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        path = v->as.str.ptr;
    }
    if (!aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }

    struct stat pst;
    if (stat(path, &pst) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot stat %s: %s", path, strerror(errno));
        return aegis_tool_result_set_string(out, buf);
    }

    glob_ctx_t gc = {.out = {0}, .pattern = pattern};
    walk_ctx_t wctx = {.token = token, .cancelled = false};

    if (S_ISREG(pst.st_mode)) {
        // Single file: match its name.
        (void)glob_visit(path, path, &gc, &(bool){false});
    } else {
        aegis_status_t rc = walk_tree(path, "", 0, glob_visit, &gc, &wctx);
        if (rc != AEGIS_OK) {
            out_buf_destroy(&gc.out);
            return rc;
        }
    }

    if (gc.out.buf == NULL) {
        out_buf_destroy(&gc.out);
        return aegis_tool_result_set_string(out, "(no matches)");
    }
    if (gc.out.truncated) {
        size_t need = gc.out.len + 32;
        char*  nbuf = (char*)realloc(gc.out.buf, need);
        if (nbuf) {
            gc.out.buf = nbuf;
            memcpy(gc.out.buf + gc.out.len, "... truncated\n", 15);
            gc.out.len += 14;
        }
    }
    aegis_status_t st = aegis_tool_result_set_string(out, gc.out.buf);
    out_buf_destroy(&gc.out);
    return st;
}

// ── grep ────────────────────────────────────────────────────────────────

typedef struct grep_ctx {
    out_buf_t   out;
    regex_t     re;
    const char* include;
    bool        re_ready;
} grep_ctx_t;

static bool file_is_binary(const char* full)
{
    FILE*         f = fopen(full, "rb");
    if (!f) {
        return true;  // unreadable: treat as binary so it is skipped
    }
    char  probe[1024];
    size_t n = fread(probe, 1, sizeof(probe), f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        if (probe[i] == '\0') {
            return true;
        }
    }
    return false;
}

static aegis_status_t grep_visit(const char* rel, const char* full, void* user, bool* stop)
{
    grep_ctx_t* gc = (grep_ctx_t*)user;
    if (gc->include && fnmatch(gc->include, path_basename(rel), 0) != 0) {
        return AEGIS_OK;
    }
    if (file_is_binary(full)) {
        return AEGIS_OK;
    }

    FILE* f = fopen(full, "rb");
    if (!f) {
        // Inline error, keep going: consistent with read tool semantics.
        char line[DISC_PATH_MAX + 128];
        snprintf(line, sizeof(line), "error: cannot open %s: %s", rel, strerror(errno));
        out_buf_append_line(&gc->out, line);
        if (gc->out.stopped) {
            *stop = true;
        }
        return AEGIS_OK;
    }

    char  linebuf[DISC_MAX_LINE];
    size_t lineno = 0;
    while (fgets(linebuf, sizeof(linebuf), f)) {
        lineno++;
        linebuf[strcspn(linebuf, "\n")] = '\0';
        regmatch_t m                    = {0};
        if (regexec(&gc->re, linebuf, 1, &m, 0) == 0) {
            char out_line[DISC_PATH_MAX + DISC_MAX_LINE];
            snprintf(out_line, sizeof(out_line), "%s:%zu: %s", rel, lineno, linebuf);
            out_buf_append_line(&gc->out, out_line);
            if (gc->out.stopped) {
                break;
            }
        }
    }
    fclose(f);
    if (gc->out.stopped) {
        *stop = true;
    }
    return AEGIS_OK;
}

static aegis_status_t tool_grep_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t*              out)
{
    (void)user;
    const aegis_tool_value_t* v       = NULL;
    const char*               pattern = NULL;
    const char*               path    = ".";
    const char*               include = NULL;
    if (aegis_tool_args_find(args, "pattern", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        pattern = v->as.str.ptr;
    }
    if (!pattern) {
        return aegis_tool_result_set_string(out, "error: missing pattern");
    }
    if (aegis_tool_args_find(args, "path", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr) {
        path = v->as.str.ptr;
    }
    if (aegis_tool_args_find(args, "include", &v) && v && v->type == AEGIS_TOOL_VAL_STRING &&
        v->as.str.ptr && v->as.str.ptr[0]) {
        include = v->as.str.ptr;
    }
    if (!aegis_safe_relative_path(path)) {
        return aegis_tool_result_set_string(out, "error: path must stay inside the project");
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    regex_t re;
    int     rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char err[256];
        regerror(rc, &re, err, sizeof(err));
        char buf[320];
        snprintf(buf, sizeof(buf), "error: invalid regex: %s", err);
        return aegis_tool_result_set_string(out, buf);
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        regfree(&re);
        char buf[256];
        snprintf(buf, sizeof(buf), "error: cannot stat %s: %s", path, strerror(errno));
        return aegis_tool_result_set_string(out, buf);
    }

    aegis_status_t result = AEGIS_OK;
    if (S_ISREG(st.st_mode)) {
        grep_ctx_t gc = {.out = {0}, .re = re, .include = include, .re_ready = true};
        bool     stop = false;
        result       = grep_visit(path, path, &gc, &stop);
        if (result == AEGIS_OK) {
            if (gc.out.buf != NULL) {
                if (gc.out.truncated) {
                    size_t need = gc.out.len + 32;
                    char*  nbuf = (char*)realloc(gc.out.buf, need);
                    if (nbuf) {
                        gc.out.buf = nbuf;
                        memcpy(gc.out.buf + gc.out.len, "... truncated\n", 15);
                        gc.out.len += 14;
                    }
                }
                result = aegis_tool_result_set_string(out, gc.out.buf);
            } else {
                result = aegis_tool_result_set_string(out, "");
            }
            out_buf_destroy(&gc.out);
        } else {
            out_buf_destroy(&gc.out);
        }
    } else {
        grep_ctx_t gc   = {.out = {0}, .re = re, .include = include, .re_ready = true};
        walk_ctx_t wctx = {.token = token, .cancelled = false};
        aegis_status_t rc2 = walk_tree(path, "", 0, grep_visit, &gc, &wctx);
        if (rc2 != AEGIS_OK) {
            out_buf_destroy(&gc.out);
            regfree(&re);
            return rc2;
        }
        if (gc.out.buf != NULL) {
            if (gc.out.truncated) {
                size_t need = gc.out.len + 32;
                char*  nbuf = (char*)realloc(gc.out.buf, need);
                if (nbuf) {
                    gc.out.buf = nbuf;
                    memcpy(gc.out.buf + gc.out.len, "... truncated\n", 15);
                    gc.out.len += 14;
                }
            }
            result = aegis_tool_result_set_string(out, gc.out.buf);
        } else {
            result = aegis_tool_result_set_string(out, "");
        }
        out_buf_destroy(&gc.out);
    }
    regfree(&re);
    return result;
}

// ── Schemas & definitions ───────────────────────────────────────────────

static const aegis_tool_param_spec_t list_params[] = {
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = false, .description = "Directory to list (default .)"},
};
static const aegis_tool_schema_t list_schema = {.params = list_params, .param_count = 1};

static const aegis_tool_param_spec_t glob_params[] = {
    {.name = "pattern", .type = AEGIS_TOOL_VAL_STRING, .required = true, .description = "Filename glob, matched recursively"},
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = false, .description = "Root directory (default .)"},
};
static const aegis_tool_schema_t glob_schema = {.params = glob_params, .param_count = 2};

static const aegis_tool_param_spec_t grep_params[] = {
    {.name = "pattern", .type = AEGIS_TOOL_VAL_STRING, .required = true, .description = "POSIX extended regex"},
    {.name = "path", .type = AEGIS_TOOL_VAL_STRING, .required = false, .description = "File or directory (default .)"},
    {.name = "include", .type = AEGIS_TOOL_VAL_STRING, .required = false, .description = "Filename filter glob, e.g. *.c"},
};
static const aegis_tool_schema_t grep_schema = {.params = grep_params, .param_count = 3};

const aegis_tool_def_t aegis_coding_tool_list = {
    .name         = "list",
    .description  = "List directory entries: name, dir/file, size; dirs first",
    .schema       = list_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_list_execute,
};

const aegis_tool_def_t aegis_coding_tool_glob = {
    .name         = "glob",
    .description  = "Recursively find files by glob pattern (results capped)",
    .schema       = glob_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_glob_execute,
};

const aegis_tool_def_t aegis_coding_tool_grep = {
    .name         = "grep",
    .description  = "Search file contents with POSIX ERE; prints path:line: text",
    .schema       = grep_schema,
    .capabilities = AEGIS_CAP_READ_FILE,
    .execute      = tool_grep_execute,
};

aegis_status_t aegis_coding_discovery_tools_register_all(aegis_tool_registry_t* reg)
{
    if (!reg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st;
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_list);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_glob);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_tool_registry_register(reg, &aegis_coding_tool_grep);
    if (st != AEGIS_OK) {
        return st;
    }
    return AEGIS_OK;
}
