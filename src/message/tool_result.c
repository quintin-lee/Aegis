/**
 * @file tool_result.c
 * @brief Message-layer tool result: owned call-id/content/error strings
 * (NULL clears on set), stored status defaulting to OK, partial-delivery
 * flag, and deep-copy clone. Destroys safely on NULL.
 */
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

/**
 * @brief Allocate a new empty tool result with status defaulting to OK.
 *
 * The caller owns the result and must call aegis_message_tool_result_destroy.
 *
 * @param[out] out Receives the new result; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
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

/**
 * @brief Destroy a tool result, freeing every owned string.
 *
 * NULL is a no-op.
 *
 * @param[in] r Tool result to destroy, or NULL.
 */
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

/**
 * @brief Deep-copy a tool result, allocating fresh strings.
 *
 * The caller owns the clone and must destroy it with aegis_message_tool_result_destroy.
 *
 * @param[in]  src Tool result to clone (must be non-NULL).
 * @param[out] out Receives the new copy; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL src/out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
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

/**
 * @brief Borrow the result's call-id string.
 *
 * @param[in] r Tool result, or NULL.
 * @return Call-id text, or NULL for NULL result.
 */
const char* aegis_message_tool_result_call_id(const aegis_message_tool_result_t* r)
{
    return r ? r->call_id : NULL;
}
/**
 * @brief Borrow the result content string.
 *
 * @param[in] r Tool result, or NULL.
 * @return Content text, or NULL for NULL result.
 */
const char* aegis_message_tool_result_content(const aegis_message_tool_result_t* r)
{
    return r ? r->content : NULL;
}
/**
 * @brief Borrow the result error string.
 *
 * @param[in] r Tool result, or NULL.
 * @return Error text, or NULL for NULL result / no error.
 */
const char* aegis_message_tool_result_error(const aegis_message_tool_result_t* r)
{
    return r ? r->error : NULL;
}
/**
 * @brief Return the result status code.
 *
 * @param[in] r Tool result, or NULL.
 * @return Status enum; returns AEGIS_ERR_INVALID for NULL.
 */
aegis_status_t aegis_message_tool_result_status(const aegis_message_tool_result_t* r)
{
    return r ? r->status : AEGIS_ERR_INVALID;
}
/**
 * @brief Return whether this result represents a partial delivery.
 *
 * @param[in] r Tool result, or NULL.
 * @return true when partial, false otherwise.
 */
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

/**
 * @brief Set (or clear) the call-id string.
 *
 * NULL clears the field. Deep-copied. Returns AEGIS_ERR_NOMEM on
 * allocation failure.
 *
 * @param[in] r  Tool result to update.
 * @param[in] id New call-id text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL r,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_tool_result_set_call_id(aegis_message_tool_result_t* r, const char* id)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->call_id, id);
}
/**
 * @brief Set (or clear) the content string.
 *
 * NULL clears the field. Deep-copied.
 *
 * @param[in] r      Tool result to update.
 * @param[in] c      New content text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL r,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_tool_result_set_content(aegis_message_tool_result_t* r, const char* c)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->content, c);
}
/**
 * @brief Set (or clear) the error string.
 *
 * NULL clears the field. Deep-copied.
 *
 * @param[in] r      Tool result to update.
 * @param[in] e      New error text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL r,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_tool_result_set_error(aegis_message_tool_result_t* r, const char* e)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&r->error, e);
}
/**
 * @brief Set the result status code.
 *
 * @param[in] r    Tool result to update.
 * @param[in] s    New status.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL r.
 */
aegis_status_t aegis_message_tool_result_set_status(aegis_message_tool_result_t* r,
                                                    aegis_status_t               s)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    r->status = s;
    return AEGIS_OK;
}
/**
 * @brief Set the partial-delivery flag.
 *
 * @param[in] r      Tool result to update.
 * @param[in] p      New flag value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL r.
 */
aegis_status_t aegis_message_tool_result_set_is_partial(aegis_message_tool_result_t* r, bool p)
{
    if (!r) {
        return AEGIS_ERR_INVALID;
    }
    r->is_partial = p;
    return AEGIS_OK;
}
