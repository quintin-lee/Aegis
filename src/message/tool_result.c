#define _POSIX_C_SOURCE 200809L
#include "aegis/message/tool_result.h"
#include <stdlib.h>
#include <string.h>

struct aegis_message_tool_result {
    char*          call_id;
    char*          content;
    char*          error;
    aegis_status_t status;
    bool           is_partial;
};

aegis_status_t aegis_message_tool_result_create(aegis_message_tool_result_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_tool_result_t* r = (aegis_message_tool_result_t*)calloc(1, sizeof(*r));
    if (!r) {
        return AEGIS_ERR_NOMEM;
    }
    r->status = AEGIS_OK;
    *out      = r;
    return AEGIS_OK;
}

void aegis_message_tool_result_destroy(aegis_message_tool_result_t* r)
{
    if (!r) {
        return;
    }
    free(r->call_id);
    free(r->content);
    free(r->error);
    free(r);
}

aegis_status_t aegis_message_tool_result_clone(const aegis_message_tool_result_t* src,
                                               aegis_message_tool_result_t**      out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_tool_result_t* dst = NULL;
    aegis_status_t               st  = aegis_message_tool_result_create(&dst);
    if (st != AEGIS_OK) {
        return st;
    }
    if (src->call_id) {
        dst->call_id = strdup(src->call_id);
        if (!dst->call_id) {
            aegis_message_tool_result_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->content) {
        dst->content = strdup(src->content);
        if (!dst->content) {
            aegis_message_tool_result_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->error) {
        dst->error = strdup(src->error);
        if (!dst->error) {
            aegis_message_tool_result_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    dst->status     = src->status;
    dst->is_partial = src->is_partial;
    *out            = dst;
    return AEGIS_OK;
}

const char* aegis_message_tool_result_call_id(const aegis_message_tool_result_t* r)
{
    return r ? r->call_id : NULL;
}
const char* aegis_message_tool_result_content(const aegis_message_tool_result_t* r)
{
    return r ? r->content : NULL;
}
const char* aegis_message_tool_result_error(const aegis_message_tool_result_t* r)
{
    return r ? r->error : NULL;
}
aegis_status_t aegis_message_tool_result_status(const aegis_message_tool_result_t* r)
{
    return r ? r->status : AEGIS_ERR_INVALID;
}
bool aegis_message_tool_result_is_partial(const aegis_message_tool_result_t* r)
{
    return r ? r->is_partial : false;
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

aegis_status_t aegis_message_tool_result_set_call_id(aegis_message_tool_result_t* r, const char* id)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->call_id, id);
}
aegis_status_t aegis_message_tool_result_set_content(aegis_message_tool_result_t* r, const char* c)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->content, c);
}
aegis_status_t aegis_message_tool_result_set_error(aegis_message_tool_result_t* r, const char* e)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->error, e);
}
aegis_status_t aegis_message_tool_result_set_status(aegis_message_tool_result_t* r,
                                                    aegis_status_t               s)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    r->status = s;
    return AEGIS_OK;
}
aegis_status_t aegis_message_tool_result_set_is_partial(aegis_message_tool_result_t* r, bool p)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    r->is_partial = p;
    return AEGIS_OK;
}
