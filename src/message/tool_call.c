/**
 * @file tool_call.c
 * @brief Tool-call model: id, tool name and JSON arguments with owned
 * heap strings, deep-copy clone and NULL-tolerant accessors.
 */
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

/**
 * @brief Allocate a new, empty tool call with index initialised to -1.
 *
 * All owned strings start NULL. The caller owns the result and must call
 * aegis_tool_call_destroy.
 *
 * @param[out] out Receives the new tool call; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
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

/**
 * @brief Destroy a tool call and all strings it owns.
 *
 * Frees @c call_id, @c tool_name, @c arguments and the struct. NULL is a
 * no-op.
 *
 * @param[in] c Tool call to destroy, or NULL.
 */
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

/**
 * @brief Deep-copy a tool call, allocating fresh strings.
 *
 * The caller owns the clone and must destroy it with aegis_tool_call_destroy.
 *
 * @param[in]  src Tool call to clone (must be non-NULL).
 * @param[out] out Receives the new copy; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL src/out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
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

/**
 * @brief Borrow the tool-call id string.
 *
 * @param[in] c Tool call, or NULL.
 * @return Call-id text, or NULL for NULL call.
 */
const char* aegis_tool_call_id(const aegis_tool_call_t* c)
{
    return c ? c->call_id : NULL;
}
/**
 * @brief Borrow the tool name string.
 *
 * @param[in] c Tool call, or NULL.
 * @return Tool name text, or NULL for NULL call.
 */
const char* aegis_tool_call_name(const aegis_tool_call_t* c)
{
    return c ? c->tool_name : NULL;
}
/**
 * @brief Borrow the JSON arguments string.
 *
 * @param[in] c Tool call, or NULL.
 * @return Arguments text, or NULL for NULL call / empty args.
 */
const char* aegis_tool_call_arguments(const aegis_tool_call_t* c)
{
    return c ? c->arguments : NULL;
}
/**
 * @brief Return the tool-call index within its parent message.
 *
 * Returns -1 when @p c is NULL or no index has been set.
 *
 * @param[in] c Tool call, or NULL.
 * @return Index value, or -1 for NULL.
 */
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

/**
 * @brief Set the call-id, replacing any prior value.
 *
 * Empty string is rejected. Deep-copied.
 *
 * @param[in] c    Tool call to update.
 * @param[in] id   New call-id text.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL c or empty id,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_call_set_id(aegis_tool_call_t* c, const char* id)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->call_id, id);
}
/**
 * @brief Set the tool name, replacing any prior value.
 *
 * Deep-copied.
 *
 * @param[in] c      Tool call to update.
 * @param[in] name   New tool name text.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL c,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_call_set_name(aegis_tool_call_t* c, const char* name)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->tool_name, name);
}
/**
 * @brief Set the JSON arguments string, replacing any prior value.
 *
 * Deep-copied.
 *
 * @param[in] c      Tool call to update.
 * @param[in] json   New arguments text.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL c,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_call_set_arguments(aegis_tool_call_t* c, const char* json)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&c->arguments, json);
}
/**
 * @brief Set the tool-call index within its parent message.
 *
 * @param[in] c    Tool call to update.
 * @param[in] idx  New index value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL c.
 */
aegis_status_t aegis_tool_call_set_index(aegis_tool_call_t* c, int idx)
{
    if (!c) {
        return AEGIS_ERR_INVALID;
    }
    c->index = idx;
    return AEGIS_OK;
}
