#define _POSIX_C_SOURCE 200809L
#include "aegis/common/time.h"
#include <time.h>
#include <stdint.h>

aegis_mono_ns_t aegis_mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (aegis_mono_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

aegis_wall_ns_t aegis_wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (aegis_wall_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void aegis_sleep_ns(uint64_t duration_ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(duration_ns / 1000000000ULL);
    ts.tv_nsec = (long)(duration_ns % 1000000000ULL);
    nanosleep(&ts, NULL);
}

void aegis_sleep_ms(uint64_t ms)
{
    aegis_sleep_ns(ms * 1000000ULL);
}

int64_t aegis_mono_elapsed(aegis_mono_ns_t start, aegis_mono_ns_t end)
{
    return end >= start ? (end - start) : 0;
}

int64_t aegis_ns_to_us(int64_t ns)
{
    return ns / 1000LL;
}

int64_t aegis_ns_to_ms(int64_t ns)
{
    return ns / 1000000LL;
}

double aegis_ns_to_sec(double ns)
{
    return ns / 1000000000.0;
}
