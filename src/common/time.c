/**
 * @file time.c
 * @brief Monotonic and wall-clock time via clock_gettime.
 *
 * aegis_mono_now() uses CLOCK_MONOTONIC (steady, unaffected by NTP).
 * aegis_wall_now() uses CLOCK_REALTIME (subject to system clock changes).
 * aegis_sleep_ns/ms use nanosleep with sub-millisecond precision.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/time.h"
#include <time.h>
#include <stdint.h>

/**
 * @brief Monotonic timestamp in nanoseconds (CLOCK_MONOTONIC, NTP-immune).
 *
 * Suitable for measuring intervals; never compare against wall-clock values.
 *
 * @return Nanoseconds since an unspecified steady origin.
 */
aegis_mono_ns_t aegis_mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (aegis_mono_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * @brief Wall-clock timestamp in nanoseconds (CLOCK_REALTIME, may jump).
 *
 * Suitable for timestamps persisted or shown to users; never use for intervals.
 *
 * @return Nanoseconds since the Unix epoch.
 */
aegis_wall_ns_t aegis_wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (aegis_wall_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * @brief Sleep for a nanosecond duration via nanosleep.
 *
 * @param duration_ns Nanoseconds to sleep (0 returns immediately).
 */
void aegis_sleep_ns(uint64_t duration_ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(duration_ns / 1000000000ULL);
    ts.tv_nsec = (long)(duration_ns % 1000000000ULL);
    nanosleep(&ts, NULL);
}

/**
 * @brief Sleep for a millisecond duration.
 *
 * @param ms Milliseconds to sleep.
 */
void aegis_sleep_ms(uint64_t ms)
{
    aegis_sleep_ns(ms * 1000000ULL);
}

/**
 * @brief Saturating monotonic interval: 0 when @p end precedes @p start.
 *
 * Guards against clock-domain mixups producing huge unsigned wrap-arounds.
 *
 * @param start Interval start (monotonic ns).
 * @param end   Interval end (monotonic ns).
 * @return end - start, or 0 when end < start.
 */
int64_t aegis_mono_elapsed(aegis_mono_ns_t start, aegis_mono_ns_t end)
{
    return end >= start ? (end - start) : 0;
}

/**
 * @brief Truncating nanoseconds-to-microseconds conversion.
 *
 * @param ns Nanoseconds.
 * @return Whole microseconds.
 */
int64_t aegis_ns_to_us(int64_t ns)
{
    return ns / 1000LL;
}

/**
 * @brief Truncating nanoseconds-to-milliseconds conversion.
 *
 * @param ns Nanoseconds.
 * @return Whole milliseconds.
 */
int64_t aegis_ns_to_ms(int64_t ns)
{
    return ns / 1000000LL;
}

/**
 * @brief Fractional nanoseconds-to-seconds conversion.
 *
 * @param ns Nanoseconds.
 * @return Seconds as a double.
 */
double aegis_ns_to_sec(double ns)
{
    return ns / 1000000000.0;
}
