#define _POSIX_C_SOURCE 200809L
#include "aegis/session/session.h"
#include "aegis/common/uuid.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

struct aegis_session {
    char                  id[37];
    char                  branch_id[37];
    char                  parent_id[37];
    char*                 project_root;
    uint64_t              created_at;
    uint64_t              updated_at;
    aegis_message_list_t* messages;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void gen_uuid_str(char out[37])
{
    aegis_uuid_t u = aegis_uuid_generate();
    aegis_uuid_format(&u, out, 37);
}

aegis_status_t aegis_session_create(const char* project_root, aegis_session_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_session_t* s = (aegis_session_t*)calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    gen_uuid_str(s->id);
    gen_uuid_str(s->branch_id);
    s->parent_id[0] = '\0';
    s->created_at   = now_ms();
    s->updated_at   = s->created_at;
    if (project_root) {
        s->project_root = strdup(project_root);
        if (!s->project_root) {
            free(s);
            return AEGIS_ERR_NOMEM;
        }
    }
    aegis_status_t st = aegis_message_list_create(&s->messages);
    if (st != AEGIS_OK) {
        free(s->project_root);
        free(s);
        return st;
    }
    *out = s;
    return AEGIS_OK;
}

void aegis_session_destroy(aegis_session_t* s)
{
    if (!s) {
        return;
    }
    free(s->project_root);
    if (s->messages) {
        aegis_message_list_destroy(s->messages);
    }
    free(s);
}

const char* aegis_session_id(const aegis_session_t* s)
{
    return s ? s->id : NULL;
}
uint64_t aegis_session_created_at(const aegis_session_t* s)
{
    return s ? s->created_at : 0;
}
uint64_t aegis_session_updated_at(const aegis_session_t* s)
{
    return s ? s->updated_at : 0;
}
const char* aegis_session_branch_id(const aegis_session_t* s)
{
    return s ? s->branch_id : NULL;
}
const char* aegis_session_parent_id(const aegis_session_t* s)
{
    return s && s->parent_id[0] ? s->parent_id : NULL;
}

aegis_status_t aegis_session_append_message(aegis_session_t* s, const aegis_message_t* msg)
{
    if (!s || !msg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = aegis_message_list_append(s->messages, msg);
    if (st == AEGIS_OK) {
        s->updated_at = now_ms();
    }
    return st;
}

size_t aegis_session_message_count(const aegis_session_t* s)
{
    return s && s->messages ? aegis_message_list_count(s->messages) : 0;
}

const aegis_message_t* aegis_session_message_at(const aegis_session_t* s, size_t idx)
{
    if (!s || !s->messages) {
        return NULL;
    }
    return aegis_message_list_at(s->messages, idx);
}

const aegis_message_list_t* aegis_session_messages(const aegis_session_t* s)
{
    return s ? s->messages : NULL;
}

aegis_status_t aegis_session_compact(aegis_session_t* s, size_t keep_messages)
{
    if (!s || !s->messages) {
        return AEGIS_ERR_INVALID;
    }
    size_t count = aegis_message_list_count(s->messages);
    if (keep_messages >= count) {
        return AEGIS_OK;
    }
    size_t start = count - keep_messages;
    size_t end   = count;
    if (start < count &&
        aegis_message_role(aegis_message_list_at(s->messages, start)) == AEGIS_MESSAGE_TOOL) {
        const char* call_id = aegis_message_tool_call_id(aegis_message_list_at(s->messages, start));
        while (start > 0) {
            const aegis_message_t* prev      = aegis_message_list_at(s->messages, start - 1);
            bool                   owns_call = false;
            for (size_t j = 0; j < aegis_message_tool_call_count(prev); ++j) {
                const aegis_tool_call_t* c = aegis_message_tool_call_at(prev, j);
                if (call_id && c && strcmp(call_id, aegis_tool_call_id(c)) == 0) {
                    owns_call = true;
                    break;
                }
            }
            if (owns_call) {
                --start;
                break;
            }
            --start;
        }
    }
    if (start < count &&
        aegis_message_tool_call_count(aegis_message_list_at(s->messages, start)) > 0 &&
        start + 1 < count &&
        aegis_message_role(aegis_message_list_at(s->messages, start + 1)) == AEGIS_MESSAGE_TOOL) {
        end = start + 1;
        while (end < count &&
               aegis_message_role(aegis_message_list_at(s->messages, end)) == AEGIS_MESSAGE_TOOL) {
            ++end;
        }
    }
    aegis_message_list_t* retained = NULL;
    if (aegis_message_list_create(&retained) != AEGIS_OK) {
        return AEGIS_ERR_NOMEM;
    }
    for (size_t i = start; i < end; ++i) {
        aegis_status_t st =
            aegis_message_list_append(retained, aegis_message_list_at(s->messages, i));
        if (st != AEGIS_OK) {
            aegis_message_list_destroy(retained);
            return st;
        }
    }
    aegis_message_list_destroy(s->messages);
    s->messages   = retained;
    s->updated_at = now_ms();
    return AEGIS_OK;
}

aegis_status_t aegis_session_fork(const aegis_session_t* src, aegis_session_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_session_t* n  = NULL;
    aegis_status_t   st = aegis_session_create(src->project_root, &n);
    if (st != AEGIS_OK) {
        return st;
    }
    // copy parent
    strncpy(n->parent_id, src->id, sizeof(n->parent_id) - 1);
    // clone messages
    aegis_message_list_destroy(n->messages);
    n->messages = NULL;
    st          = aegis_message_list_clone(src->messages, &n->messages);
    if (st != AEGIS_OK) {
        aegis_session_destroy(n);
        return st;
    }
    *out = n;
    return AEGIS_OK;
}

// JSONL persistence — simple, one JSON object per line
// We use a minimal JSON escaping for content

static void json_escape(FILE* f, const char* s)
{
    if (!s) {
        return;
    }
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
        }
        if (*p == '\n') {
            fputs("\\n", f);
            continue;
        }
        if (*p == '\r') {
            fputs("\\r", f);
            continue;
        }
        fputc(*p, f);
    }
}

aegis_status_t aegis_session_save(const aegis_session_t* s, const char* path)
{
    if (!s || !path) {
        return AEGIS_ERR_INVALID;
    }
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        return AEGIS_ERR_INVALID;
    }
    // session header
    fprintf(f,
            "{\"type\":\"session_start\",\"v\":1,\"id\":\"%s\",\"branch\":\"%s\",\"parent\":\"%s\","
            "\"created\":%llu,\"project\":\"",
            s->id, s->branch_id, s->parent_id, (unsigned long long)s->created_at);
    json_escape(f, s->project_root ? s->project_root : "");
    fputs("\"}\n", f);
    size_t n = aegis_session_message_count(s);
    for (size_t i = 0; i < n; i++) {
        const aegis_message_t* m       = aegis_session_message_at(s, i);
        const char*            role    = aegis_message_role_str(aegis_message_role(m));
        const char*            content = aegis_message_content(m) ? aegis_message_content(m) : "";
        const char*            id      = aegis_message_id(m) ? aegis_message_id(m) : "";
        fprintf(f, "{\"type\":\"message\",\"id\":\"%s\",\"role\":\"%s\",\"content\":\"", id, role);
        json_escape(f, content);
        const char* reasoning = aegis_message_reasoning(m);
        if (reasoning) {
            fputs(",\"reasoning\":\"", f);
            json_escape(f, reasoning);
        }
        fputs("\"}\n", f);
        // tool calls if any
        size_t tc = aegis_message_tool_call_count(m);
        for (size_t j = 0; j < tc; j++) {
            const aegis_tool_call_t* c    = aegis_message_tool_call_at(m, j);
            const char*              cid  = aegis_tool_call_id(c) ? aegis_tool_call_id(c) : "";
            const char*              name = aegis_tool_call_name(c) ? aegis_tool_call_name(c) : "";
            const char* args = aegis_tool_call_arguments(c) ? aegis_tool_call_arguments(c) : "";
            fprintf(f, "{\"type\":\"tool_call\",\"msg_id\":\"%s\",\"call_id\":\"%s\",\"name\":\"",
                    id, cid);
            json_escape(f, name);
            fputs("\",\"index\":", f);
            fprintf(f, "%d,\"args\":\"", aegis_tool_call_index(c));
            json_escape(f, args);
            fputs("\"}\n", f);
        }
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return AEGIS_ERR_INVALID;
    }
    return AEGIS_OK;
}  // Loader restores session metadata, messages, and tool calls from JSONL
aegis_status_t aegis_session_load(const char* path, aegis_session_t** out)
{
    if (!path || !out) {
        return AEGIS_ERR_INVALID;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        return AEGIS_ERR_NOT_FOUND;
    }
    aegis_session_t* s = NULL;
    char             line[8192];
    // peek first line for project
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return AEGIS_ERR_INVALID;
    }
    // parse project from session_start (naive)
    char        project[1024] = "";
    const char* p             = strstr(line, "\"project\":\"");
    if (p) {
        p += 11;
        size_t idx = 0;
        while (*p && *p != '"' && idx < sizeof(project) - 1) {
            if (*p == '\\' && *(p + 1)) {
                p++;
            }
            project[idx++] = *p++;
        }
        project[idx] = '\0';
    }
    aegis_status_t st = aegis_session_create(project[0] ? project : NULL, &s);
    if (st != AEGIS_OK) {
        fclose(f);
        return st;
    }
    // override id/branch from file if present
    p = strstr(line, "\"id\":\"");
    if (p) {
        p += strlen("\"id\":\"");
        size_t k = 0;
        while (*p && *p != '"' && k < 36) {
            s->id[k++] = *p++;
        }
        s->id[k] = '\0';
    }
    p = strstr(line, "\"branch\":\"");
    if (p) {
        p += strlen("\"branch\":\"");
        size_t k = 0;
        while (*p && *p != '"' && k < 36) {
            s->branch_id[k++] = *p++;
        }
        s->branch_id[k] = '\0';
    }

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"type\":\"tool_call\"")) {
            char        msg_id[64] = "", call_id[128] = "", name[256] = "", args[4096] = "";
            const char* q = strstr(line, "\"msg_id\":\"");
            if (q) {
                q += strlen("\"msg_id\":\"");
                size_t k = 0;
                while (*q && *q != '"' && k < sizeof(msg_id) - 1) {
                    msg_id[k++] = *q++;
                }
                msg_id[k] = 0;
            }
            q = strstr(line, "\"call_id\":\"");
            if (q) {
                q += strlen("\"call_id\":\"");
                size_t k = 0;
                while (*q && *q != '"' && k < sizeof(call_id) - 1) {
                    call_id[k++] = *q++;
                }
                call_id[k] = 0;
            }
            q = strstr(line, "\"name\":\"");
            if (q) {
                q += strlen("\"name\":\"");
                size_t k = 0;
                while (*q && *q != '"' && k < sizeof(name) - 1) {
                    name[k++] = *q++;
                }
                name[k] = 0;
            }
            q = strstr(line, "\"args\":\"");
            if (q) {
                q += strlen("\"args\":\"");
                size_t k = 0;
                while (*q && *q != '"' && k < sizeof(args) - 1) {
                    if (*q == '\\' && q[1]) {
                        ++q;
                        args[k++] = *q++;
                    } else {
                        args[k++] = *q++;
                    }
                }
                args[k] = 0;
            }
            int index = -1;
            q         = strstr(line, "\"index\":");
            if (q) {
                index = atoi(q + strlen("\"index\":"));
            }
            for (size_t i = 0; i < aegis_session_message_count(s); ++i) {
                aegis_message_t* m = (aegis_message_t*)aegis_session_message_at(s, i);
                if (m && strcmp(aegis_message_id(m), msg_id) == 0) {
                    aegis_tool_call_t* c = NULL;
                    if (aegis_tool_call_create(&c) == AEGIS_OK &&
                        aegis_tool_call_set_id(c, call_id) == AEGIS_OK &&
                        aegis_tool_call_set_name(c, name) == AEGIS_OK &&
                        aegis_tool_call_set_arguments(c, args) == AEGIS_OK &&
                        aegis_tool_call_set_index(c, index) == AEGIS_OK) {
                        aegis_message_add_tool_call(m, c);
                    }
                    aegis_tool_call_destroy(c);
                    break;
                }
            }
        } else if (strstr(line, "\"type\":\"message\"")) {
            // parse role and content
            char        role_str[16] = "";
            const char* rp           = strstr(line, "\"role\":\"");
            if (rp) {
                rp += 8;
                size_t k = 0;
                while (*rp && *rp != '"' && k < 15) {
                    role_str[k++] = *rp++;
                }
                role_str[k] = '\0';
            }
            char        message_id[64] = "";
            const char* mip            = strstr(line, "\"id\":\"");
            if (mip) {
                mip += strlen("\"id\":\"");
                size_t k = 0;
                while (*mip && *mip != '"' && k + 1 < sizeof(message_id)) {
                    message_id[k++] = *mip++;
                }
                message_id[k] = 0;
            }
            char        content[4096] = "";
            const char* cp            = strstr(line, "\"content\":\"");
            if (cp) {
                cp += 11;
                size_t k = 0;
                while (*cp && *cp != '"' && k < 4095) {
                    if (*cp == '\\' && *(cp + 1) == 'n') {
                        content[k++] = '\n';
                        cp += 2;
                        continue;
                    }
                    if (*cp == '\\' && *(cp + 1)) {
                        cp++;
                    }
                    content[k++] = *cp++;
                }
            }
            /* Optional reasoning (absent in sessions saved before it existed). */
            char        reasoning_buf[8192] = "";
            int         has_reasoning       = 0;
            const char* rp2                 = strstr(line, "\"reasoning\":\"");
            if (rp2) {
                has_reasoning = 1;
                rp2 += strlen("\"reasoning\":\"");
                size_t k = 0;
                while (*rp2 && *rp2 != '"' && k < sizeof(reasoning_buf) - 1) {
                    if (*rp2 == '\\' && *(rp2 + 1) == 'n') {
                        reasoning_buf[k++] = '\n';
                        rp2 += 2;
                        continue;
                    }
                    if (*rp2 == '\\' && *(rp2 + 1)) {
                        rp2++;
                    }
                    reasoning_buf[k++] = *rp2++;
                }
            }
            aegis_message_role_t role = AEGIS_MESSAGE_USER;
            if (strcmp(role_str, "system") == 0) {
                role = AEGIS_MESSAGE_SYSTEM;
            } else if (strcmp(role_str, "assistant") == 0) {
                role = AEGIS_MESSAGE_ASSISTANT;
            } else if (strcmp(role_str, "tool") == 0) {
                role = AEGIS_MESSAGE_TOOL;
            } else if (strcmp(role_str, "event") == 0) {
                role = AEGIS_MESSAGE_EVENT;
            }
            aegis_message_t* m = NULL;
            if (aegis_message_create(role, &m) == AEGIS_OK) {
                const char* mid = strstr(line, "\"id\":\"");
                if (mid) {
                    mid += strlen("\"id\":\"");
                    char   idbuf[64] = "";
                    size_t k         = 0;
                    while (*mid && *mid != '"' && k < sizeof(idbuf) - 1) {
                        idbuf[k++] = *mid++;
                    }
                    idbuf[k] = 0;
                    aegis_message_set_id(m, idbuf);
                }
                if (message_id[0]) {
                    aegis_message_set_id(m, message_id);
                }
                aegis_message_set_content(m, content);
                if (has_reasoning) {
                    aegis_message_set_reasoning(m, reasoning_buf);
                }
                aegis_session_append_message(s, m);
                aegis_message_destroy(m);
            }
        }
    }
    fclose(f);
    *out = s;
    return AEGIS_OK;
}
