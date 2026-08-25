#ifndef AEGIS_TIME_H
#define AEGIS_TIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file time.h
 * @brief Monotonic and wall-clock time utilities.
 *
 * All durations and timestamps are expressed in nanoseconds unless
 * otherwise noted. Monotonic time is suitable for measuring elapsed
 * intervals; wall-clock time is suitable for absolute timestamps.
 */

/**
 * @brief High-resolution monotonic clock (steady, never jumps backward).
 *
 * Suitable for measuring elapsed time, timeouts, and intervals.
 * Value is only meaningful relative to other monotonic timestamps
 * taken on the same process invocation.
 */
typedef int64_t aegis_mono_ns_t;

/**
 * @brief Wall-clock time in nanoseconds since the Unix epoch (1970-01-01).
 *
 * May jump backward or forward due to NTP adjustments. Use
 * aegis_mono_now() for interval measurement instead.
 */
typedef int64_t aegis_wall_ns_t;

/**
 * @brief Return the current monotonic time in nanoseconds.
 *
 * @return Current monotonic timestamp.
 */
aegis_mono_ns_t aegis_mono_now(void);

/**
 * @brief Return the current wall-clock time in nanoseconds since epoch.
 *
 * @return Current wall-clock timestamp.
 */
aegis_wall_ns_t aegis_wall_now(void);

/**
 * @brief Sleep for at least @p duration_ns nanoseconds.
 *
 * The actual sleep duration may be longer due to OS scheduling,
 * but will never be shorter than requested.
 *
 * @param duration_ns Duration to sleep (non-negative).
 */
void aegis_sleep_ns(uint64_t duration_ns);

/**
 * @brief Sleep for @p ms milliseconds.
 *
 * Equivalent to aegis_sleep_ns(ms * 1'000'000).
 *
 * @param ms Duration in milliseconds (non-negative).
 */
void aegis_sleep_ms(uint64_t ms);

/**
 * @brief Compute the elapsed nanoseconds between two monotonic timestamps.
 *
 * Returns 0 if @p end < @p start (clock went backward, which should
 * not happen on a correctly functioning system).
 *
 * @param start Timestamp at the beginning of the interval.
 * @param end   Timestamp at the end of the interval.
 * @return Elapsed nanoseconds (non-negative).
 */
int64_t aegis_mono_elapsed(aegis_mono_ns_t start, aegis_mono_ns_t end);

/**
 * @brief Convert nanoseconds to microseconds.
 *
 * @param ns Nanoseconds.
 * @return Microseconds (truncated toward zero).
 */
int64_t aegis_ns_to_us(int64_t ns);

/**
 * @brief Convert nanoseconds to milliseconds.
 *
 * @param ns Nanoseconds.
 * @return Milliseconds (truncated toward zero).
 */
int64_t aegis_ns_to_ms(int64_t ns);

/**
 * @brief Convert nanoseconds to seconds as a double.
 *
 * @param ns Nanoseconds.
 * @return Seconds with sub-nanosecond precision.
 */
double aegis_ns_to_sec(double ns);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TIME_H */
