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
 * All durations are in nanoseconds unless noted.
 */

/** High-resolution monotonic clock (steady, never jumps backward). */
typedef int64_t aegis_mono_ns_t;

/** Wall-clock time in nanoseconds since Unix epoch. */
typedef int64_t aegis_wall_ns_t;

/**
 * @brief Return current monotonic time in nanoseconds.
 */
aegis_mono_ns_t aegis_mono_now(void);

/**
 * @brief Return current wall-clock time in nanoseconds since epoch.
 */
aegis_wall_ns_t aegis_wall_now(void);

/**
 * @brief Sleep for at least duration_ns nanoseconds.
 *
 * May sleep longer due to OS scheduling, but never shorter.
 */
void aegis_sleep_ns(uint64_t duration_ns);

/**
 * @brief Sleep for milliseconds.
 */
void aegis_sleep_ms(uint64_t ms);

/**
 * @brief Compute elapsed nanoseconds between two monotonic timestamps.
 *
 * Returns 0 if end < start (clock went backward — should not happen).
 */
int64_t aegis_mono_elapsed(aegis_mono_ns_t start, aegis_mono_ns_t end);

/**
 * @brief Convert nanoseconds to microseconds.
 */
int64_t aegis_ns_to_us(int64_t ns);

/**
 * @brief Convert nanoseconds to milliseconds.
 */
int64_t aegis_ns_to_ms(int64_t ns);

/**
 * @brief Convert nanoseconds to seconds (double precision).
 */
double aegis_ns_to_sec(double ns);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TIME_H */
