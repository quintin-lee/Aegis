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

/** Immutable error object: code plus message plus borrowed cause link. */
struct aegis_error {
    aegis_err_t          code;                     /**< Machine-readable error code. */
    char                 msg[AEGIS_ERROR_MSG_MAX]; /**< Human-readable message (NUL-terminated). */
    const aegis_error_t* cause; /**< Borrowed cause, NULL when none (not owned). */
};

/**
 * @brief Create an error with a printf-style message.
 *
 * A NULL @p fmt yields an empty message. The handle is heap-allocated
 * and must be destroyed by the caller.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param code Error code.
 * @param fmt  printf-style format (no trailing newline; may be NULL).
 * @param ...  Format arguments for @p fmt.
 * @return AEGIS_ERROR_NONE on success, AEGIS_ERROR_NOMEM on allocation failure.
 */
aegis_err_t aegis_error_new(aegis_error_t** out, aegis_err_t code, const char* fmt, ...)
{
    if (!out) {
        return (aegis_err_t)-1;
    }
    aegis_error_t* err = calloc(1, sizeof(*err));
    if (!err) {
        return AEGIS_ERROR_NOMEM;
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
    return AEGIS_ERROR_NONE;
}

/**
 * @brief Create an error linked to a borrowed cause.
 *
 * The cause is borrowed, not cloned: the caller keeps owning (and
 * destroying) it. A NULL cause degrades to aegis_error_new behavior.
 *
 * @param[out] out   Receives the handle (ownership: transferred).
 * @param code  Error code.
 * @param cause Causing error (borrowed; may be NULL).
 * @param fmt   printf-style format (no trailing newline; may be NULL).
 * @param ...   Format arguments for @p fmt.
 * @return AEGIS_OK on success.
 */
aegis_err_t aegis_error_new_cause(aegis_error_t** out, aegis_err_t code, const aegis_error_t* cause,
                                  const char* fmt, ...)
{
    if (!out) {
        return (aegis_err_t)-1;
    }
    aegis_error_t* err = calloc(1, sizeof(*err));
    if (!err) {
        return AEGIS_ERROR_NOMEM;
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
    return AEGIS_ERROR_NONE;
}

/**
 * @brief Shallow-copy an error object (message bytes plus borrowed cause link).
 *
 * The cause pointer is copied as-is (still borrowed); the chain itself
 * is not deep-cloned.
 *
 * @param src Error to clone (borrowed; NULL is rejected).
 * @param[out] out Receives the copy (ownership: transferred).
 * @return AEGIS_OK on success, AEGIS_ERROR_INVALID on NULL args, AEGIS_ERROR_NOMEM on failure.
 */
aegis_err_t aegis_error_clone(const aegis_error_t* src, aegis_error_t** out)
{
    if (!src || !out) {
        return AEGIS_ERROR_INVALID;
    }
    aegis_error_t* copy = calloc(1, sizeof(*copy));
    if (!copy) {
        return AEGIS_ERROR_NOMEM;
    }
    *copy = *src;
    *out  = copy;
    return AEGIS_ERROR_NONE;
}

/**
 * @brief Destroy an error object only (borrowed causes are left alone).
 *
 * Safe to call with NULL (no-op).
 *
 * @param err Handle to destroy (ownership: consumed).
 */
void aegis_error_destroy(aegis_error_t* err)
{
    free(err);
}

/**
 * @brief Read the error code (AEGIS_ERROR_NONE for NULL input).
 *
 * @param err Handle (borrowed).
 * @return Error code.
 */
aegis_err_t aegis_error_code(const aegis_error_t* err)
{
    return err ? err->code : AEGIS_ERROR_NONE;
}

/**
 * @brief Read the message ("" for NULL input; never returns NULL).
 *
 * @param err Handle (borrowed).
 * @return Message string (borrowed, owned by the error object).
 */
const char* aegis_error_message(const aegis_error_t* err)
{
    return err ? err->msg : "";
}

/**
 * @brief Read the borrowed cause link (NULL when the chain ends).
 *
 * @param err Handle (borrowed).
 * @return Cause error (borrowed), or NULL.
 */
const aegis_error_t* aegis_error_cause(const aegis_error_t* err)
{
    return err ? err->cause : NULL;
}

/** Thread-local slot holding the calling thread's last-error detail. */
static _Thread_local aegis_error_t* tls_last_error = NULL;

/**
 * @brief Publish an error as the calling thread's last-error detail.
 *
 * Consumes @p err (NULL clears the slot); any previously stored error is
 * destroyed. Each thread has an independent slot.
 *
 * @param err Detail to store (ownership: consumed; NULL clears).
 */
void aegis_error_set_last(aegis_error_t* err)
{
    aegis_error_destroy(tls_last_error);
    tls_last_error = err;
}

/**
 * @brief Read the calling thread's last-error detail.
 *
 * @return Borrowed pointer (valid until the next set on this thread), or NULL when empty.
 */
const aegis_error_t* aegis_error_last(void)
{
    return tls_last_error;
}

/**
 * @brief Serialize the error chain into @p buf, one level per " caused by: " segment.
 *
 * A NULL @p err writes "". Output is always NUL-terminated when @p maxlen > 0;
 * overlong chains are truncated but the return still reports the would-be length.
 *
 * @param buf    Output buffer (must hold at least @p maxlen bytes).
 * @param maxlen Size of @p buf in bytes.
 * @param err    Root error (borrowed; may be NULL).
 * @return Characters that would have been written excluding the NUL, or 0 on NULL buf / zero
 * maxlen.
 */
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
