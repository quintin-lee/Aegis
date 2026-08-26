/**
 * @file test_task_concurrent.c
 * @brief Concurrent stress test for task graph operations.
 */
#include "aegis/graph.h"
#include "aegis/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static int g_failures = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void fail(const char* fmt, ...) {
    pthread_mutex_lock(&g_lock);
    g_failures++;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    pthread_mutex_unlock(&g_lock);
}

typedef struct {
    int id;
} arg_t;

static void* worker(void* arg) {
    arg_t* a = (arg_t*)arg;
    for (int i = 0; i < 50; i++) {
        aegis_task_graph_t* g = NULL;
        if (aegis_task_graph_create(&g) != AEGIS_OK) {
            fail("Thread %d iter %d: graph create failed\n", a->id, i);
            continue;
        }

        aegis_task_t* tasks[5];
        for (int j = 0; j < 5; j++) {
            char name[16];
            snprintf(name, sizeof(name), "t%d", j);
            if (aegis_task_create(&tasks[j], name, NULL) != AEGIS_OK) {
                fail("Thread %d iter %d: task create failed\n", a->id, i);
                aegis_task_graph_destroy(g);
                goto next;
            }
            if (aegis_task_graph_add_task(g, tasks[j]) != AEGIS_OK) {
                fail("Thread %d iter %d: add task failed\n", a->id, i);
                aegis_task_graph_destroy(g);
                goto next;
            }
        }

        /* Build: T0→T1→T2, T0→T3, T2+T3→T4 */
        if (aegis_task_graph_add_dependency(g, tasks[0], tasks[1]) != AEGIS_OK) {
            fail("Thread %d iter %d: add dep 0→1 failed\n", a->id, i);
        }
        if (aegis_task_graph_add_dependency(g, tasks[1], tasks[2]) != AEGIS_OK) {
            fail("Thread %d iter %d: add dep 1→2 failed\n", a->id, i);
        }
        if (aegis_task_graph_add_dependency(g, tasks[0], tasks[3]) != AEGIS_OK) {
            fail("Thread %d iter %d: add dep 0→3 failed\n", a->id, i);
        }
        if (aegis_task_graph_add_dependency(g, tasks[2], tasks[4]) != AEGIS_OK) {
            fail("Thread %d iter %d: add dep 2→4 failed\n", a->id, i);
        }
        if (aegis_task_graph_add_dependency(g, tasks[3], tasks[4]) != AEGIS_OK) {
            fail("Thread %d iter %d: add dep 3→4 failed\n", a->id, i);
        }

        /* Validate */
        if (!aegis_task_graph_is_dag(g)) {
            fail("Thread %d iter %d: not a DAG\n", a->id, i);
        }
        if (aegis_task_graph_validate(g) != AEGIS_OK) {
            fail("Thread %d iter %d: validation failed\n", a->id, i);
        }

        /* Query */
        aegis_task_t** ready = NULL;
        size_t cnt = 0;
        if (aegis_task_graph_ready_tasks(g, &ready, &cnt) != AEGIS_OK) {
            fail("Thread %d iter %d: ready_tasks failed\n", a->id, i);
        } else {
            free(ready);
        }

        /* Remove a task — caller now owns it */
        aegis_task_graph_remove_task(g, tasks[0]);
        aegis_task_destroy(tasks[0]);

        aegis_task_graph_destroy(g);

    next:
        ;
    }
    free(a);
    return NULL;
}

int main(void) {
    const int N = 8;
    pthread_t threads[N];

    for (int i = 0; i < N; i++) {
        arg_t* a = malloc(sizeof(*a));
        if (!a) { perror("malloc"); return 1; }
        a->id = i;
        if (pthread_create(&threads[i], NULL, worker, a) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < N; i++) {
        pthread_join(threads[i], NULL);
    }

    if (g_failures > 0) {
        fprintf(stderr, "FAILED: %d concurrent failures\n", g_failures);
        return 1;
    }

    printf("task graph concurrency test passed\n");
    return 0;
}
