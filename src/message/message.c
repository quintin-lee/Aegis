#define _POSIX_C_SOURCE 200809L
#include "aegis/message/message.h"
#include "aegis/common/uuid.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Internal layout
struct aegis_message {
    char*                id;
    aegis_message_role_t role;
    uint64_t             timestamp;  // ms since epoch
    char*                content;
    char*                reasoning;
    char*                tool_call_id;  // for TOOL role: which call this result corresponds to
    char*                parent_id;

    // tool_calls attached to assistant message
    aegis_tool_call_t** tool_calls;
    size_t              tool_call_count;
    size_t              tool_call_cap;
};

struct aegis_message_list {
    aegis_message_t** msgs;
    size_t            count;
    size_t            cap;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static char* gen_id(void)
{
    aegis_uuid_t u = aegis_uuid_generate();
    char         buf[37];
    aegis_uuid_format(&u, buf, sizeof(buf));
    return strdup(buf);
}

aegis_status_t aegis_message_create(aegis_message_role_t role, aegis_message_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_t* m = (aegis_message_t*)calloc(1, sizeof(*m));
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    m->role      = role;
    m->timestamp = now_ms();
    m->id        = gen_id();
    if (!m->id) {
        free(m);
        return AEGIS_ERR_NOMEM;
    }
    *out = m;
    return AEGIS_OK;
}

void aegis_message_destroy(aegis_message_t* m)
{
    if (!m) {
        return;
    }
    free(m->id);
    free(m->content);
    free(m->reasoning);
    free(m->tool_call_id);
    free(m->parent_id);
    for (size_t i = 0; i < m->tool_call_count; i++) {
        aegis_tool_call_destroy(m->tool_calls[i]);
    }
    free(m->tool_calls);
    free(m);
}

aegis_status_t aegis_message_clone(const aegis_message_t* src, aegis_message_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_t* dst = NULL;
    aegis_status_t   st  = aegis_message_create(src->role, &dst);
    if (st != AEGIS_OK) {
        return st;
    }
    free(dst->id);
    dst->id = src->id ? strdup(src->id) : NULL;
    if (src->id && !dst->id) {
        aegis_message_destroy(dst);
        return AEGIS_ERR_NOMEM;
    }
    dst->timestamp = src->timestamp;
    if (src->content) {
        dst->content = strdup(src->content);
        if (!dst->content) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->reasoning) {
        dst->reasoning = strdup(src->reasoning);
        if (!dst->reasoning) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->tool_call_id) {
        dst->tool_call_id = strdup(src->tool_call_id);
        if (!dst->tool_call_id) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->parent_id) {
        dst->parent_id = strdup(src->parent_id);
        if (!dst->parent_id) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    for (size_t i = 0; i < src->tool_call_count; i++) {
        aegis_tool_call_t* c = NULL;
        st                   = aegis_tool_call_clone(src->tool_calls[i], &c);
        if (st != AEGIS_OK) {
            aegis_message_destroy(dst);
            return st;
        }
        st = aegis_message_add_tool_call(dst, c);
        aegis_tool_call_destroy(c);  // add clones
        if (st != AEGIS_OK) {
            aegis_message_destroy(dst);
            return st;
        }
    }
    *out = dst;
    return AEGIS_OK;
}

/* accessors */
const char* aegis_message_id(const aegis_message_t* m)
{
    return m ? m->id : NULL;
}
aegis_message_role_t aegis_message_role(const aegis_message_t* m)
{
    return m ? m->role : AEGIS_MESSAGE_USER;
}
uint64_t aegis_message_timestamp(const aegis_message_t* m)
{
    return m ? m->timestamp : 0;
}
const char* aegis_message_content(const aegis_message_t* m)
{
    return m ? m->content : NULL;
}
const char* aegis_message_reasoning(const aegis_message_t* m)
{
    return m ? m->reasoning : NULL;
}
const char* aegis_message_tool_call_id(const aegis_message_t* m)
{
    return m ? m->tool_call_id : NULL;
}
const char* aegis_message_parent_id(const aegis_message_t* m)
{
    return m ? m->parent_id : NULL;
}
size_t aegis_message_tool_call_count(const aegis_message_t* m)
{
    return m ? m->tool_call_count : 0;
}
const aegis_tool_call_t* aegis_message_tool_call_at(const aegis_message_t* m, size_t idx)
{
    if (!m || idx >= m->tool_call_count) {
        return NULL;
    }
    return m->tool_calls[idx];
}

static aegis_status_t set_str(char** dst, const char* src)
{
    char* n = NULL;
    if (src) {
        n = strdup(src);
        if (!n) {
            return AEGIS_ERR_NOMEM;
        }
    }
    free(*dst);
    *dst = n;
    return AEGIS_OK;
}

aegis_status_t aegis_message_set_id(aegis_message_t* m, const char* id)
{
    if (!m || !id || !id[0]) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->id, id);
}

aegis_status_t aegis_message_set_content(aegis_message_t* m, const char* txt)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->content, txt);
}
aegis_status_t aegis_message_set_reasoning(aegis_message_t* m, const char* r)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->reasoning, r);
}
aegis_status_t aegis_message_set_tool_call_id(aegis_message_t* m, const char* id)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->tool_call_id, id);
}
aegis_status_t aegis_message_set_parent_id(aegis_message_t* m, const char* pid)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->parent_id, pid);
}

aegis_status_t aegis_message_add_tool_call(aegis_message_t* m, const aegis_tool_call_t* call)
{
    if (!m || !call) {
        return AEGIS_ERR_INVALID;
    }
    if (m->tool_call_count >= m->tool_call_cap) {
        size_t              ncap = m->tool_call_cap ? m->tool_call_cap * 2 : 4;
        aegis_tool_call_t** n    = (aegis_tool_call_t**)realloc(m->tool_calls, ncap * sizeof(*n));
        if (!n) {
            return AEGIS_ERR_NOMEM;
        }
        m->tool_calls    = n;
        m->tool_call_cap = ncap;
    }
    aegis_tool_call_t* c  = NULL;
    aegis_status_t     st = aegis_tool_call_clone(call, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    m->tool_calls[m->tool_call_count++] = c;
    return AEGIS_OK;
}

/* ── list ───────────────────────────────────────────────────────────────── */

aegis_status_t aegis_message_list_create(aegis_message_list_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_list_t* l = (aegis_message_list_t*)calloc(1, sizeof(*l));
    if (!l) {
        return AEGIS_ERR_NOMEM;
    }
    *out = l;
    return AEGIS_OK;
}

void aegis_message_list_destroy(aegis_message_list_t* l)
{
    if (!l) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        aegis_message_destroy(l->msgs[i]);
    }
    free(l->msgs);
    free(l);
}

aegis_status_t aegis_message_list_clone(const aegis_message_list_t* src, aegis_message_list_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_list_t* dst = NULL;
    aegis_status_t        st  = aegis_message_list_create(&dst);
    if (st != AEGIS_OK) {
        return st;
    }
    for (size_t i = 0; i < src->count; i++) {
        st = aegis_message_list_append(dst, src->msgs[i]);
        if (st != AEGIS_OK) {
            aegis_message_list_destroy(dst);
            return st;
        }
    }
    *out = dst;
    return AEGIS_OK;
}

size_t aegis_message_list_count(const aegis_message_list_t* l)
{
    return l ? l->count : 0;
}
const aegis_message_t* aegis_message_list_at(const aegis_message_list_t* l, size_t idx)
{
    if (!l || idx >= l->count) {
        return NULL;
    }
    return l->msgs[idx];
}

static aegis_status_t list_reserve(aegis_message_list_t* l, size_t need)
{
    if (l->count + need <= l->cap) {
        return AEGIS_OK;
    }
    size_t ncap = l->cap ? l->cap * 2 : 8;
    while (ncap < l->count + need) {
        ncap *= 2;
    }
    aegis_message_t** n = (aegis_message_t**)realloc(l->msgs, ncap * sizeof(*n));
    if (!n) {
        return AEGIS_ERR_NOMEM;
    }
    l->msgs = n;
    l->cap  = ncap;
    return AEGIS_OK;
}

aegis_status_t aegis_message_list_append(aegis_message_list_t* l, const aegis_message_t* msg)
{
    if (!l || !msg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = list_reserve(l, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_message_t* c = NULL;
    st                 = aegis_message_clone(msg, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    l->msgs[l->count++] = c;
    return AEGIS_OK;
}

aegis_status_t aegis_message_list_prepend(aegis_message_list_t* l, const aegis_message_t* msg)
{
    if (!l || !msg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = list_reserve(l, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_message_t* c = NULL;
    st                 = aegis_message_clone(msg, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    memmove(&l->msgs[1], &l->msgs[0], l->count * sizeof(*l->msgs));
    l->msgs[0] = c;
    l->count++;
    return AEGIS_OK;
}
