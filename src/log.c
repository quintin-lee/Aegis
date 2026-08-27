/**
 * @file log.c
 * @brief Structured logging implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Globals ───────────────────────────────────────────────────────────────── */

static aegis_log_sink_fn g_sink      = NULL;
static void*             g_sink_user = NULL;
static aegis_log_level_t g_min_level = AEGIS_LOG_DEBUG;
static pthread_mutex_t   g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread-local format buffer — 4KB per thread. */
#if defined(__clang__) || defined(__GNUC__)
#define TLS __thread
#else
#define TLS
#endif

static TLS char g_log_buf[4096];

/* ── Sink accessors ────────────────────────────────────────────────────────── */

static aegis_log_sink_fn get_sink(void)
{
    aegis_log_sink_fn s;
    pthread_mutex_lock(&g_log_mutex);
    s = g_sink;
    pthread_mutex_unlock(&g_log_mutex);
    return s;
}

static void* get_sink_user(void)
{
    void* u;
    pthread_mutex_lock(&g_log_mutex);
    u = g_sink_user;
    pthread_mutex_unlock(&g_log_mutex);
    return u;
}

static aegis_log_level_t get_min_level(void)
{
    aegis_log_level_t l;
    pthread_mutex_lock(&g_log_mutex);
    l = g_min_level;
    pthread_mutex_unlock(&g_log_mutex);
    return l;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void aegis_log_set_sink(aegis_log_sink_fn sink, void* user)
{
    pthread_mutex_lock(&g_log_mutex);
    g_sink      = sink;
    g_sink_user = user;
    pthread_mutex_unlock(&g_log_mutex);
}

void aegis_log_set_min_level(aegis_log_level_t level)
{
    pthread_mutex_lock(&g_log_mutex);
    g_min_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

aegis_log_level_t aegis_log_get_min_level(void)
{
    return get_min_level();
}

const char* aegis_log_level_str(aegis_log_level_t level)
{
    switch (level) {
    case AEGIS_LOG_DEBUG:
        return "DEBUG";
    case AEGIS_LOG_INFO:
        return "INFO ";
    case AEGIS_LOG_WARN:
        return "WARN ";
    case AEGIS_LOG_ERROR:
        return "ERROR";
    case AEGIS_LOG_FATAL:
        return "FATAL";
    default:
        return "?????";
    }
}

void aegis_log_impl(aegis_log_level_t level, const char* module, const char* ctx, const char* fmt,
                    ...)
{
    /* Fast path: check level before allocating anything. */
    if (level < get_min_level()) {
        return;
    }

    aegis_log_sink_fn sink = get_sink();
    if (!sink) {
        return;
    }

    /* Format message into thread-local buffer. */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log_buf, sizeof(g_log_buf), fmt, ap);
    va_end(ap);
    g_log_buf[sizeof(g_log_buf) - 1] = '\0';

    /* Build a single formatted string for the sink. */
    static const char* prefix_fmt = "[%s] [%s] %s%s%s";
    static char        prefixed[512];
    int n = snprintf(prefixed, sizeof(prefixed), prefix_fmt, aegis_log_level_str(level),
                     module ? module : "?", ctx ? ctx : "", ctx ? ": " : "", g_log_buf);
    if (n < 0 || (size_t)n >= sizeof(prefixed)) {
        prefixed[sizeof(prefixed) - 1] = '\0';
    }

    sink(level, module, ctx, prefixed, get_sink_user());
}
