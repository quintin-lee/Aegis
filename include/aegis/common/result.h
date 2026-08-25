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
 * Result is heap-allocated and opaque. Use aegis_result_create_ok/err to
 * construct, and query/access functions to inspect.
 *
 * Usage:
 *   aegis_result_t *r = aegis_result_create_ok((void*)42);
 *   if (aegis_result_is_ok(r)) { ... }
 *   aegis_result_destroy(r);
 */

/** Opaque result handle. */
typedef struct aegis_result aegis_result_t;

/**
 * @brief Create an Ok result holding a payload pointer.
 *
 * @param payload  Value to hold (caller retains ownership).
 * @return         New result on success, NULL on failure.
 */
aegis_result_t* aegis_result_create_ok(void* payload);

/**
 * @brief Create an Err result owning an error.
 *
 * @param err  Error to own (transferred).
 * @return     New result on success, NULL on failure.
 */
aegis_result_t* aegis_result_create_err(aegis_error_t* err);

/**
 * @brief Create an Err result from a code and message.
 */
aegis_result_t* aegis_result_create_errf(aegis_err_t code, const char* fmt, ...);

/**
 * @brief Check if result is Ok.
 */
bool aegis_result_is_ok(const aegis_result_t* r);

/**
 * @brief Check if result is Err.
 */
bool aegis_result_is_err(const aegis_result_t* r);

/**
 * @brief Return the payload pointer (only valid when Ok).
 */
void* aegis_result_get(const aegis_result_t* r);

/**
 * @brief Return the error (only valid when Err).
 */
const aegis_error_t* aegis_result_err_get(const aegis_result_t* r);

/**
 * @brief Take ownership of the error and destroy the result.
 *
 * @return The error on Err, NULL on Ok. Caller owns returned error.
 */
aegis_error_t* aegis_result_take_err(aegis_result_t* r);

/**
 * @brief Destroy the result.
 */
void aegis_result_destroy(aegis_result_t* r);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_RESULT_H */
