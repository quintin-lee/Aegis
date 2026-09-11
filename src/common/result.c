/**
 * @file result.c
 * @brief Result<T> heap-allocated handle implementation.
 *
 * Ownership semantics:
 * - aegis_result_create_ok: payload ownership stays with caller.
 * - aegis_result_create_err / create_errf: error ownership transfers into result.
 * - aegis_result_destroy: frees the handle and any owned error.
 * - aegis_result_take_err: transfers error ownership out of the result.
 */
#include "aegis/common/result.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** Heap result handle: Ok carries a borrowed payload, Err owns an error. */
struct aegis_result {
    int            ok;      /**< 1 = Ok, 0 = Err, -1 = destroyed (use-after-free tripwire). */
    void*          payload; /**< Borrowed payload, valid when ok. */
    aegis_error_t* error;   /**< Owned error, valid when !ok. */
};

/**
 * @brief Create an Ok result wrapping a caller-owned payload.
 *
 * @param payload Payload pointer stored borrowed (may be NULL).
 * @return New handle (ownership: transferred), or NULL on allocation failure.
 */
aegis_result_t* aegis_result_create_ok(void* payload)
{
    aegis_result_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->ok      = 1;
    r->payload = payload;
    return r;
}

/**
 * @brief Create an Err result taking ownership of @p err.
 *
 * @param err Error to own (ownership: transferred; may be NULL for an empty Err).
 * @return New handle (ownership: transferred), or NULL on allocation failure.
 */
aegis_result_t* aegis_result_create_err(aegis_error_t* err)
{
    aegis_result_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->ok    = 0;
    r->error = err;
    return r;
}

/**
 * @brief Create an Err result from a freshly formatted error.
 *
 * Builds the error via aegis_error_new and transfers it into the result,
 * so the caller owns nothing on return. A NULL @p fmt yields an empty message.
 *
 * @param code Error code for the new error.
 * @param fmt  printf-style format (may be NULL).
 * @param ...  Format arguments for @p fmt.
 * @return New Err handle (ownership: transferred), or NULL on allocation failure.
 */
aegis_result_t* aegis_result_create_errf(aegis_err_t code, const char* fmt, ...)
{
    aegis_error_t* err = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        aegis_error_new(&err, code, fmt, ap);
        va_end(ap);
    } else {
        aegis_error_new(&err, code, NULL);
    }
    if (!err) {
        return NULL;
    }
    return aegis_result_create_err(err);
}

/**
 * @brief Test whether the result is Ok (NULL counts as not-Ok).
 *
 * @param r Handle (borrowed).
 * @return true when Ok, false otherwise.
 */
bool aegis_result_is_ok(const aegis_result_t* r)
{
    return r && r->ok;
}

/**
 * @brief Test whether the result is Err (NULL counts as not-Err).
 *
 * @param r Handle (borrowed).
 * @return true when Err, false otherwise.
 */
bool aegis_result_is_err(const aegis_result_t* r)
{
    return r && !r->ok;
}

/**
 * @brief Borrow the Ok payload (NULL unless the result is Ok).
 *
 * @param r Handle (borrowed).
 * @return Borrowed payload, or NULL.
 */
void* aegis_result_get(const aegis_result_t* r)
{
    return r && r->ok ? r->payload : NULL;
}

/**
 * @brief Borrow the owned error (NULL unless the result is Err).
 *
 * Ownership stays with the result; use aegis_result_take_err to move it out.
 *
 * @param r Handle (borrowed).
 * @return Borrowed error, or NULL.
 */
const aegis_error_t* aegis_result_err_get(const aegis_result_t* r)
{
    return r && !r->ok ? r->error : NULL;
}

/**
 * @brief Move the owned error out, leaving an empty Ok result behind.
 *
 * After the take the result holds no error and reports Ok, so a later
 * destroy will not double-free.
 *
 * @param r Handle (borrowed).
 * @return Owned error (ownership: transferred), or NULL when Ok/NULL input.
 */
aegis_error_t* aegis_result_take_err(aegis_result_t* r)
{
    if (!r || r->ok) {
        return NULL;
    }
    aegis_error_t* e = r->error;
    r->error         = NULL;
    r->ok            = 1;
    return e;
}

/**
 * @brief Destroy a result and its owned error (payload stays caller-owned).
 *
 * The ok flag is poisoned to -1 before free as a use-after-free tripwire.
 * Safe to call with NULL (no-op).
 *
 * @param r Handle to destroy (ownership: consumed).
 */
void aegis_result_destroy(aegis_result_t* r)
{
    if (!r) {
        return;
    }
    if (!r->ok && r->error) {
        aegis_error_destroy(r->error);
        r->error = NULL;
    }
    r->ok = -1;
    free(r);
}
