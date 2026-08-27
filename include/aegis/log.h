/**
 * @file log.h
 * @brief Structured logging with level, module, and context support.
 *
 * The logging subsystem provides:
 *   - Hierarchical log levels (DEBUG < INFO < WARN < ERROR < FATAL)
 *   - Module namespacing (e.g., "runtime", "scheduler", "executor")
 *   - Context propagation (arbitrary key-value pairs)
 *   - A global sink callback for output routing
 *
 * Design principles:
 *   - Core modules MUST NOT call printf/fprintf directly for observability.
 *   - All log calls go through aegis_log_*() which routes to the configured sink.
 *   - Log messages are formatted once and passed as immutable strings to the sink.
 *   - The sink is free to drop, buffer, or forward messages asynchronously.
 *   - Log entries do NOT retain references to caller-owned objects; all data is
 *     copied or formatted into a static buffer before the sink is called.
 */
#ifndef AEGIS_LOG_H
#define AEGIS_LOG_H

#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Log level ─────────────────────────────────────────────────────────────── */

/**
 * @brief Log severity level.
 *
 * Ordered from lowest to highest severity. Filters can be set to ignore
 * levels below a threshold.
 */
typedef enum aegis_log_level {
    AEGIS_LOG_DEBUG = 0, /**< Detailed diagnostic information.      */
    AEGIS_LOG_INFO  = 1, /**< General operational information.      */
    AEGIS_LOG_WARN  = 2, /**< Unexpected but non-fatal conditions.  */
    AEGIS_LOG_ERROR = 3, /**< Errors that may prevent operation.    */
    AEGIS_LOG_FATAL = 4, /**< Fatal errors requiring immediate halt.*/
} aegis_log_level_t;

/* ── Sink callback ─────────────────────────────────────────────────────────── */

/**
 * @brief Log sink callback signature.
 *
 * The sink receives a formatted log message and must not retain any
 * pointers beyond the duration of this call. All string data is valid
 * only for the lifetime of the callback.
 *
 * @param level      Log level of the message.
 * @param module     Module name (e.g., "runtime", "scheduler").
 * @param context    Arbitrary context string (may be NULL).
 * @param message    Formatted log message.
 * @param user       Opaque user context passed at sink registration.
 */
typedef void (*aegis_log_sink_fn)(aegis_log_level_t level, const char* module, const char* context,
                                  const char* message, void* user);

/* ── Public API ────────────────────────────────────────────────────────────── */

/**
 * @brief Register a log sink.
 *
 * Replaces any previously registered sink. The sink is called synchronously
 * from the logging thread; it must not block indefinitely.
 *
 * Thread-safe: atomic swap of the sink pointer.
 *
 * @param sink   Sink callback (may be NULL to unregister).
 * @param user   Opaque context passed to the sink on each call.
 */
void aegis_log_set_sink(aegis_log_sink_fn sink, void* user);

/**
 * @brief Set the minimum log level.
 *
 * Messages below this level are silently dropped. Default is DEBUG (all).
 *
 * Thread-safe.
 *
 * @param level Minimum level to emit.
 */
void aegis_log_set_min_level(aegis_log_level_t level);

/**
 * @brief Get the current minimum log level.
 *
 * @return Current minimum level.
 */
aegis_log_level_t aegis_log_get_min_level(void);

/* ── Logging macros ────────────────────────────────────────────────────────── */

/**
 * @brief Log a message at the given level from the given module.
 *
 * Usage:
 *   AEGIS_LOG(AEGIS_LOG_INFO, "runtime", NULL, "Agent started: %s", name);
 *
 * @param level  Log level.
 * @param module Module name (string literal preferred).
 * @param ctx    Context string (may be NULL).
 * @param fmt    Format string (printf-style).
 */
#define AEGIS_LOG(level, module, ctx, ...) aegis_log_impl((level), (module), (ctx), __VA_ARGS__)

/* ── Low-level implementation ──────────────────────────────────────────────── */

/**
 * @brief Internal log implementation. Do not call directly; use AEGIS_LOG.
 *
 * Formats the message into a thread-local buffer and calls the sink if
 * the level meets the minimum threshold.
 *
 * @param level  Log level.
 * @param module Module name.
 * @param ctx    Context string.
 * @param fmt    Format string.
 * @param ...    Format arguments.
 */
void aegis_log_impl(aegis_log_level_t level, const char* module, const char* ctx, const char* fmt,
                    ...);

/**
 * @brief Convert a log level to a human-readable string.
 *
 * @param level Log level.
 * @return Static string; do not free.
 */
const char* aegis_log_level_str(aegis_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LOG_H */
