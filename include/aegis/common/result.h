#ifndef AEGIS_RESULT_H
#define AEGIS_RESULT_H

#include "aegis/common/error.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file result.h
 * @brief Result<T> type for explicit success/failure propagation.
 *
 * Mirrors Rust's Result<T, E>. The handle is heap-allocated and opaque.
 *
 * Usage:
 *   aegis_result_t *r = aegis_result_create_ok((void*)42);
 *   if (aegis_result_is_ok(r)) { ... }
 *   aegis_result_destroy(r);
 *
 * Ownership:
 * - aegis_result_create_ok: caller retains payload ownership.
 * - aegis_result_create_err: caller transfers error ownership into the result.
 * - aegis_result_destroy: frees the result handle and any owned error.
 */

/** Opaque result handle. */
typedef struct aegis_result aegis_result_t;

/**
 * @brief Create an Ok result holding a payload pointer.
 *
 * @param payload  Value to hold (caller retains ownership; result only stores the pointer).
 * @return         New result on success, NULL on allocation failure.
 */
aegis_result_t* aegis_result_create_ok(void* payload);

/**
 * @brief Create an Err result owning the provided error.
 *
 * @param err  Error to own (transferred; result will destroy it on free).
 * @return     New result on success, NULL on allocation failure.
 */
aegis_result_t* aegis_result_create_err(aegis_error_t* err);

/**
 * @brief Create an Err result from a code and printf-style message.
 *
 * @param code  Error code.
 * @param fmt   printf-style format string (no trailing newline).
 * @param ...   Format arguments.
 * @return      New result on success, NULL on failure.
 */
aegis_result_t* aegis_result_create_errf(aegis_err_t code, const char* fmt, ...);

/**
 * @brief Check whether the result represents success (Ok).
 *
 * @param r  Result handle (borrowed; may be NULL → returns false).
 * @return true if Ok, false if Err or NULL.
 */
bool aegis_result_is_ok(const aegis_result_t* r);

/**
 * @brief Check whether the result represents failure (Err).
 *
 * @param r  Result handle (borrowed; may be NULL → returns true).
 * @return true if Err, false if Ok or NULL.
 */
bool aegis_result_is_err(const aegis_result_t* r);

/**
 * @brief Return the payload pointer (only valid when Ok).
 *
 * @param r  Result handle (borrowed).
 * @return Payload on Ok, NULL on Err or if @p r is NULL.
 */
void* aegis_result_get(const aegis_result_t* r);

/**
 * @brief Return the error handle (only valid when Err).
 *
 * The error remains owned by the result — do NOT free the returned
 * pointer independently. Use aegis_result_take_err() to extract
 * ownership.
 *
 * @param r  Result handle (borrowed).
 * @return Error handle on Err, NULL on Ok or if @p r is NULL.
 */
const aegis_error_t* aegis_result_err_get(const aegis_result_t* r);

/**
 * @brief Take ownership of the error and destroy the result.
 *
 * Only valid when the result is Err. Returns NULL when the result
 * is Ok.
 *
 * @param r  Result handle (consumed — caller must not use after this call).
 * @return Owned error handle on Err, NULL on Ok.
 */
aegis_error_t* aegis_result_take_err(aegis_result_t* r);

/**
 * @brief Destroy the result and release all owned resources.
 *
 * If the result is Err, the owned error is also destroyed.
 * Safe to call with NULL (no-op).
 *
 * @param r  Result handle (ownership: consumed).
 */
void aegis_result_destroy(aegis_result_t* r);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_RESULT_H */
