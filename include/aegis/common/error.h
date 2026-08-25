#ifndef AEGIS_ERROR_H
#define AEGIS_ERROR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file error.h
 * @brief Structured error reporting with cause chaining.
 *
 * Errors carry a code, an optional message, and an optional chain
 * of cause errors. All fields are immutable after creation, making
 * error objects safe to share across threads.
 *
 * Memory layout:
 *   aegis_error_t  — root cause entry
 *     └─ cause     — next error in chain (borrowed, not owned)
 *
 * Ownership: aegis_error_new transfers ownership of the returned
 * handle to the caller, who must call aegis_error_destroy when done.
 */

/** Maximum message length including the null terminator. */
#define AEGIS_ERROR_MSG_MAX 512

/** Opaque error object. */
typedef struct aegis_error aegis_error_t;

/**
 * @brief Error codes — kept in sync with aegis_status_t.
 *
 * Extends aegis_status_t with error-domain-specific codes (IO,
 * format, overflow, empty) that do not appear in the core status enum.
 */
typedef enum aegis_err {
    AEGIS_ERR_NONE      =  0, /**< No error.                               */
    AEGIS_ERR_UNKNOWN   = -1, /**< Unknown / unclassified error.             */
    AEGIS_ERR_NOMEM     = -2, /**< Out of memory.                            */
    AEGIS_ERR_INVALID   = -3, /**< Invalid argument.                         */
    AEGIS_ERR_NOT_FOUND = -4, /**< Resource not found.                       */
    AEGIS_ERR_BUSY      = -5, /**< Resource is busy.                         */
    AEGIS_ERR_TIMEOUT   = -6, /**< Operation timed out.                      */
    AEGIS_ERR_CANCELLED = -7, /**< Operation was cancelled.                  */
    AEGIS_ERR_PERM      = -8, /**< Permission denied.                        */
    AEGIS_ERR_PROVIDER  = -9, /**< Error from an external provider.          */
    AEGIS_ERR_TOOL      = -10,/**< Error from a tool execution.              */
    AEGIS_ERR_IO        = -11,/**< I/O error (read / write / network).       */
    AEGIS_ERR_FORMAT    = -12,/**< Malformed input format.                   */
    AEGIS_ERR_OVERFLOW  = -13,/**< Value exceeded representable range.       */
    AEGIS_ERR_EMPTY     = -14,/**< Operation on an empty collection.         */
} aegis_err_t;

/**
 * @brief Create an error from a code and a printf-style message.
 *
 * @param[out] out       Receives the error handle. Ownership: transferred.
 * @param[in]  code      Error code.
 * @param[in]  fmt       printf-style format string (no trailing newline).
 * @param[in]  ...       Format arguments corresponding to @p fmt.
 * @return AEGIS_OK (0) on success, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_err_t aegis_error_new(aegis_error_t** out, aegis_err_t code, const char* fmt, ...);

/**
 * @brief Create an error with an explicit cause chain.
 *
 * The @p cause is borrowed (not owned) — the caller remains
 * responsible for destroying it.
 *
 * @param[out] out       Receives the error handle. Ownership: transferred.
 * @param[in]  code      Error code.
 * @param[in]  cause     Causing error (borrowed; may be NULL).
 * @param[in]  fmt       printf-style format string (no trailing newline).
 * @param[in]  ...       Format arguments.
 * @return AEGIS_OK on success.
 */
aegis_err_t aegis_error_new_cause(aegis_error_t** out, aegis_err_t code,
                                  const aegis_error_t* cause,
                                  const char* fmt, ...);

/**
 * @brief Deep-copy an error including its cause chain.
 *
 * @param[in]  src  Error to clone (borrowed; may be NULL — returns AEGIS_ERR_INVALID).
 * @param[out] out  Receives the cloned error. Ownership: transferred.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_err_t aegis_error_clone(const aegis_error_t* src, aegis_error_t** out);

/**
 * @brief Destroy an error and free all memory.
 *
 * Note: only the error object itself is freed; cause errors are
 * borrowed and must be destroyed separately by their owners.
 *
 * Safe to call with NULL (no-op).
 *
 * @param err Handle to destroy (ownership: consumed).
 */
void aegis_error_destroy(aegis_error_t* err);

/**
 * @brief Get the error code.
 *
 * @param err Error handle (borrowed; may be NULL → returns AEGIS_ERR_NONE).
 * @return Error code.
 */
aegis_err_t aegis_error_code(const aegis_error_t* err);

/**
 * @brief Get the human-readable message.
 *
 * Returns "" if @p err is NULL. Never returns NULL.
 *
 * @param err Error handle (borrowed).
 * @return Message string (statically allocated).
 */
const char* aegis_error_message(const aegis_error_t* err);

/**
 * @brief Get the cause error, or NULL if none.
 *
 * @param err Error handle (borrowed).
 * @return Cause error (borrowed), or NULL.
 */
const aegis_error_t* aegis_error_cause(const aegis_error_t* err);

/**
 * @brief Serialize the entire error chain into @p buf.
 *
 * Each level is printed on its own line, separated by " caused by: ".
 * At most @p maxlen bytes are written, including the null terminator.
 *
 * @param buf    Output buffer (must hold at least maxlen bytes).
 * @param maxlen Size of @p buf in bytes.
 * @param err    Root error (borrowed; may be NULL → writes "").
 * @return Number of characters that would have been written (excluding nul),
 *         or a negative value if @p buf is NULL or maxlen is 0.
 */
int aegis_error_chain_snprintf(char* buf, size_t maxlen, const aegis_error_t* err);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ERROR_H */
