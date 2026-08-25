/**
 * @file error.c
 * @brief Error object implementation with cause chaining.
 *
 * Each aegis_error_t carries a code, a formatted message, and an optional
 * borrowed cause pointer. Errors are immutable after creation and may be
 * safely shared across threads.
 *
 * Ownership: aegis_error_new transfers ownership of the returned handle;
 * aegis_error_destroy frees it (but NOT the cause chain, which is borrowed).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct aegis_error {
    aegis_err_t          code;
    char                 msg[AEGIS_ERROR_MSG_MAX];
    const aegis_error_t* cause; /* borrowed, not owned */
};

aegis_err_t aegis_error_new(aegis_error_t** out, aegis_err_t code, const char* fmt, ...)
{
    if (!out) {
        return (aegis_err_t)-1;
    }
    aegis_error_t* err = calloc(1, sizeof(*err));
    if (!err) {
        return AEGIS_ERR_NOMEM;
    }
    err->code  = code;
    err->cause = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        va_end(ap);
    } else {
        err->msg[0] = '\0';
    }
    *out = err;
    return AEGIS_ERR_NONE;
}

aegis_err_t aegis_error_new_cause(aegis_error_t** out, aegis_err_t code, const aegis_error_t* cause,
                                  const char* fmt, ...)
{
    if (!out) {
        return (aegis_err_t)-1;
    }
    aegis_error_t* err = calloc(1, sizeof(*err));
    if (!err) {
        return AEGIS_ERR_NOMEM;
    }
    err->code  = code;
    err->cause = cause;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        va_end(ap);
    } else {
        err->msg[0] = '\0';
    }
    *out = err;
    return AEGIS_ERR_NONE;
}

aegis_err_t aegis_error_clone(const aegis_error_t* src, aegis_error_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_error_t* copy = calloc(1, sizeof(*copy));
    if (!copy) {
        return AEGIS_ERR_NOMEM;
    }
    *copy = *src;
    *out  = copy;
    return AEGIS_ERR_NONE;
}

void aegis_error_destroy(aegis_error_t* err)
{
    free(err);
}

aegis_err_t aegis_error_code(const aegis_error_t* err)
{
    return err ? err->code : AEGIS_ERR_NONE;
}

const char* aegis_error_message(const aegis_error_t* err)
{
    return err ? err->msg : "";
}

const aegis_error_t* aegis_error_cause(const aegis_error_t* err)
{
    return err ? err->cause : NULL;
}

int aegis_error_chain_snprintf(char* buf, size_t maxlen, const aegis_error_t* err)
{
    if (!buf || maxlen == 0) {
        return 0;
    }
    buf[0]     = '\0';
    size_t off = 0;
    while (err && off < maxlen) {
        int n = (int)snprintf(buf + off, maxlen - off, "%s%s",
                              off == 0 ? "" : " caused by: ", aegis_error_message(err));
        if (n < 0) {
            break;
        }
        off += (size_t)n;
        err = aegis_error_cause(err);
    }
    return (int)off;
}
