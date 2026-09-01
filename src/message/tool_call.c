#define _POSIX_C_SOURCE 200809L
#include "aegis/message/tool_call.h"
#include <stdlib.h>
#include <string.h>

struct aegis_tool_call {
    char* call_id;
    char* tool_name;
    char* arguments;  // JSON string, owned
    int   index;
};

aegis_status_t aegis_tool_call_create(aegis_tool_call_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_tool_call_t* c = (aegis_tool_call_t*)calloc(1, sizeof(*c));
    if (!c) {
        return AEGIS_ERR_NOMEM;
    }
    c->index = -1;
    *out     = c;
    return AEGIS_OK;
}

void aegis_tool_call_destroy(aegis_tool_call_t* c)
{
    if (!c) {
        return;
    }
    free(c->call_id);
    free(c->tool_name);
    free(c->arguments);
    free(c);
}

aegis_status_t aegis_tool_call_clone(const aegis_tool_call_t* src, aegis_tool_call_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_tool_call_t* dst = NULL;
    aegis_status_t     st  = aegis_tool_call_create(&dst);
    if (st != AEGIS_OK) {
        return st;
    }
    if (src->call_id) {
        dst->call_id = strdup(src->call_id);
        if (!dst->call_id) {
            aegis_tool_call_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->tool_name) {
        dst->tool_name = strdup(src->tool_name);
        if (!dst->tool_name) {
            aegis_tool_call_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->arguments) {
        dst->arguments = strdup(src->arguments);
        if (!dst->arguments) {
            aegis_tool_call_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    dst->index = src->index;
    *out       = dst;
    return AEGIS_OK;
}

const char* aegis_tool_call_id(const aegis_tool_call_t* c)
{
    return c ? c->call_id : NULL;
}
const char* aegis_tool_call_name(const aegis_tool_call_t* c)
{
    return c ? c->tool_name : NULL;
}
const char* aegis_tool_call_arguments(const aegis_tool_call_t* c)
{
    return c ? c->arguments : NULL;
}
int aegis_tool_call_index(const aegis_tool_call_t* c)
{
    return c ? c->index : -1;
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

aegis_status_t aegis_tool_call_set_id(aegis_tool_call_t* c, const char* id)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->call_id, id);
}
aegis_status_t aegis_tool_call_set_name(aegis_tool_call_t* c, const char* name)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->tool_name, name);
}
aegis_status_t aegis_tool_call_set_arguments(aegis_tool_call_t* c, const char* json)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->arguments, json);
}
aegis_status_t aegis_tool_call_set_index(aegis_tool_call_t* c, int idx)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    c->index = idx;
    return AEGIS_OK;
}
