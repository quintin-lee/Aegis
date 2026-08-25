#ifndef AEGIS_ERROR_H
#define AEGIS_ERROR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file error.h
 * @brief Structured error reporting with context chaining.
 *
 * Errors carry a code, an optional message, and a chain of cause errors.
 * All fields are immutable after creation (thread-safe sharing permitted).
 */

/** Maximum message length including null terminator. */
#define AEGIS_ERROR_MSG_MAX 512

/** Opaque error object. */
typedef struct aegis_error aegis_error_t;

/**
 * @brief Error codes — keep in sync with aegis_status_t.
 */
typedef enum aegis_err {
    AEGIS_ERR_NONE      = 0,
    AEGIS_ERR_UNKNOWN   = -1,
    AEGIS_ERR_NOMEM     = -2,
    AEGIS_ERR_INVALID   = -3,
    AEGIS_ERR_NOT_FOUND = -4,
    AEGIS_ERR_BUSY      = -5,
    AEGIS_ERR_TIMEOUT   = -6,
    AEGIS_ERR_CANCELLED = -7,
    AEGIS_ERR_PERM      = -8,
    AEGIS_ERR_PROVIDER  = -9,
    AEGIS_ERR_TOOL      = -10,
    AEGIS_ERR_IO        = -11,
    AEGIS_ERR_FORMAT    = -12,
    AEGIS_ERR_OVERFLOW  = -13,
    AEGIS_ERR_EMPTY     = -14,
} aegis_err_t;

/**
 * @brief Create an error from a code and a formatted message.
 *
 * @param[out] out       Receives the error handle. Ownership: transferred.
 * @param[in]  code      Error code.
 * @param[in]  fmt       printf-style format string (no newline).
 * @param[in]  ...       Format arguments.
 * @return AEGIS_OK on success.
 */
aegis_err_t aegis_error_new(aegis_error_t** out, aegis_err_t code, const char* fmt, ...);

/**
 * @brief Create an error with an explicit cause chain.
 *
 * @param[out] out       Receives the error handle.
 * @param[in]  code      Error code.
 * @param[in]  cause     Causing error (borrowed; may be NULL).
 * @param[in]  fmt       Format string.
 * @param[in]  ...       Format arguments.
 */
aegis_err_t aegis_error_new_cause(aegis_error_t** out, aegis_err_t code, const aegis_error_t* cause,
                                  const char* fmt, ...);

/**
 * @brief Clone an error (deep copy).
 */
aegis_err_t aegis_error_clone(const aegis_error_t* src, aegis_error_t** out);

/**
 * @brief Destroy an error and all chained causes.
 */
void aegis_error_destroy(aegis_error_t* err);

/**
 * @brief Get the error code.
 */
aegis_err_t aegis_error_code(const aegis_error_t* err);

/**
 * @brief Get the human-readable message.
 *
 * Returns "" if err is NULL. Never returns NULL.
 */
const char* aegis_error_message(const aegis_error_t* err);

/**
 * @brief Get the cause error, or NULL if none.
 */
const aegis_error_t* aegis_error_cause(const aegis_error_t* err);

/**
 * @brief Serialize the entire error chain into buf.
 *
 * Each level is printed on its own line separated by " caused by: ".
 * Writes at most maxlen bytes including the null terminator.
 *
 * @return Number of characters that would have been written (excluding nul).
 */
int aegis_error_chain_snprintf(char* buf, size_t maxlen, const aegis_error_t* err);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ERROR_H */
