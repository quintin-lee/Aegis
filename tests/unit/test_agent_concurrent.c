/**
 * @file test_agent_concurrent.c
 * @brief Concurrent stress test for agent state machine.
 */
#include "aegis/agent/agent.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int             g_concurrent_failures = 0;
static pthread_mutex_t g_fail_lock           = PTHREAD_MUTEX_INITIALIZER;

static void record_failure(const char* fmt, ...)
{
    pthread_mutex_lock(&g_fail_lock);
    g_concurrent_failures++;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    pthread_mutex_unlock(&g_fail_lock);
}

typedef struct {
    int id;
} thread_arg_t;

static void* concurrent_agent_worker(void* arg)
{
    thread_arg_t* ta         = (thread_arg_t*)arg;
    int           iterations = 50;

    for (int i = 0; i < iterations; i++) {
        aegis_agent_t* agent = NULL;

        if (aegis_agent_create(&agent, "stress") != AEGIS_OK) {
            record_failure("Thread %d iter %d: create failed\n", ta->id, i);
            continue;
        }

        if (aegis_agent_init(agent) != AEGIS_OK) {
            record_failure("Thread %d iter %d: init failed\n", ta->id, i);
            aegis_agent_destroy(agent);
            continue;
        }

        if (aegis_agent_start(agent) != AEGIS_OK) {
            record_failure("Thread %d iter %d: start failed\n", ta->id, i);
            aegis_agent_destroy(agent);
            continue;
        }

        if (aegis_agent_pause(agent) != AEGIS_OK) {
            record_failure("Thread %d iter %d: pause failed\n", ta->id, i);
            aegis_agent_destroy(agent);
            continue;
        }

        if (aegis_agent_resume(agent) != AEGIS_OK) {
            record_failure("Thread %d iter %d: resume failed\n", ta->id, i);
            aegis_agent_destroy(agent);
            continue;
        }

        if (aegis_agent_cancel(agent) != AEGIS_OK) {
            record_failure("Thread %d iter %d: cancel failed\n", ta->id, i);
            aegis_agent_destroy(agent);
            continue;
        }

        aegis_agent_destroy(agent);
    }

    free(ta);
    return NULL;
}

int main(void)
{
    const int n_threads = 8;
    pthread_t threads[8];

    for (int i = 0; i < n_threads; i++) {
        thread_arg_t* arg = malloc(sizeof(thread_arg_t));
        if (!arg) {
            fprintf(stderr, "malloc failed\n");
            return 1;
        }
        arg->id = i;
        if (pthread_create(&threads[i], NULL, concurrent_agent_worker, arg) != 0) {
            free(arg);
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < n_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (g_concurrent_failures > 0) {
        fprintf(stderr, "FAILED: %d concurrent failures\n", g_concurrent_failures);
        return 1;
    }

    printf("agent concurrency test passed\n");
    return 0;
}
