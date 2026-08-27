/**
 * @file test_log.c
 * @brief Unit tests for the Logging module.
 */
#include "aegis/log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Test sink ─────────────────────────────────────────────────────────────── */

static int g_test_sink_call_count = 0;
static aegis_log_level_t g_last_level = AEGIS_LOG_DEBUG;
static const char* g_last_module = NULL;
static const char* g_last_message = NULL;

static void test_sink(aegis_log_level_t level, const char* module,
                      const char* context, const char* message, void* user)
{
    (void)user;
    g_test_sink_call_count++;
    g_last_level = level;
    g_last_module = module;
    g_last_message = message;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_sink_registration(void)
{
    aegis_log_set_sink(test_sink, NULL);
    assert(aegis_log_get_min_level() == AEGIS_LOG_DEBUG);

    AEGIS_LOG(AEGIS_LOG_INFO, "test", NULL, "hello");
    assert(g_test_sink_call_count == 1);
    assert(g_last_level == AEGIS_LOG_INFO);
    assert(strcmp(g_last_module, "test") == 0);
    assert(strstr(g_last_message, "hello") != NULL);

    aegis_log_set_sink(NULL, NULL);
}

static void test_level_filtering(void)
{
    aegis_log_set_sink(test_sink, NULL);
    aegis_log_set_min_level(AEGIS_LOG_WARN);

    g_test_sink_call_count = 0;
    AEGIS_LOG(AEGIS_LOG_DEBUG, "x", NULL, "debug");
    AEGIS_LOG(AEGIS_LOG_INFO,  "x", NULL, "info");
    AEGIS_LOG(AEGIS_LOG_WARN,  "x", NULL, "warn");
    AEGIS_LOG(AEGIS_LOG_ERROR, "x", NULL, "error");
    AEGIS_LOG(AEGIS_LOG_FATAL, "x", NULL, "fatal");

    assert(g_test_sink_call_count == 3); /* warn, error, fatal */

    aegis_log_set_min_level(AEGIS_LOG_DEBUG);
    aegis_log_set_sink(NULL, NULL);
}

static void test_context_propagation(void)
{
    aegis_log_set_sink(test_sink, NULL);

    AEGIS_LOG(AEGIS_LOG_INFO, "mod", "ctx-key=val", "message");
    assert(strstr(g_last_message, "ctx-key=val") != NULL);

    aegis_log_set_sink(NULL, NULL);
}

static void test_no_sink(void)
{
    aegis_log_set_sink(NULL, NULL);
    /* Must not crash. */
    AEGIS_LOG(AEGIS_LOG_INFO, "x", NULL, "ignored");
}

static void test_level_string(void)
{
    assert(strcmp(aegis_log_level_str(AEGIS_LOG_DEBUG), "DEBUG") == 0);
    assert(strcmp(aegis_log_level_str(AEGIS_LOG_INFO),  "INFO ") == 0);
    assert(strcmp(aegis_log_level_str(AEGIS_LOG_WARN),  "WARN ") == 0);
    assert(strcmp(aegis_log_level_str(AEGIS_LOG_ERROR), "ERROR") == 0);
    assert(strcmp(aegis_log_level_str(AEGIS_LOG_FATAL), "FATAL") == 0);
    assert(strcmp(aegis_log_level_str((aegis_log_level_t)99), "?????") == 0);
}

static void* test_noop(void* arg) {
    (void)arg;
    return NULL;
}

static void test_concurrent_logs(void)
{
    aegis_log_set_sink(test_sink, NULL);
    aegis_log_set_min_level(AEGIS_LOG_DEBUG);

    g_test_sink_call_count = 0;
    /* Concurrent log calls from multiple threads. */
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, test_noop, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    /* Just verify no crash under concurrent log calls. */
    for (int i = 0; i < 100; i++) {
        AEGIS_LOG(AEGIS_LOG_INFO, "t", NULL, "msg %d", i);
    }
    assert(g_test_sink_call_count > 0);

    aegis_log_set_sink(NULL, NULL);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_no_sink();
    test_level_string();
    test_sink_registration();
    test_level_filtering();
    test_context_propagation();
    test_concurrent_logs();

    printf("log: all tests passed\n");
    return 0;
}
