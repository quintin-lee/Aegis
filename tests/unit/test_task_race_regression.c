/**
 * @file test_task_race_regression.c
 * @brief Regression test for data race in g_next_task_id.
 *
 * Before fix (plain uint32_t): concurrent aegis_task_create() causes
 * duplicate task IDs → graph cycle detection gets confused → segfault.
 *
 * After fix (_Atomic uint32_t): all IDs are unique even under heavy
 * concurrent creation. Test must pass without any duplicate IDs.
 */
#include "aegis/graph.h"
#include "aegis/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS   8
#define NITER      50
#define NTASKS_PER 5

static int g_failures = 0;

typedef struct {
    int id;
} arg_t;

/* Track all created task IDs across threads to detect duplicates */
static uint32_t* g_all_ids = NULL;
static size_t    g_id_count = 0;
static size_t    g_id_cap   = 0;
static pthread_mutex_t g_ids_lock = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* arg) {
    arg_t* a = (arg_t*)arg;
    for (int i = 0; i < NITER; i++) {
        aegis_task_graph_t* g = NULL;
        if (aegis_task_graph_create(&g) != AEGIS_OK) {
            __sync_fetch_and_or(&g_failures, 1);
            continue;
        }

        aegis_task_t* tasks[NTASKS_PER];
        for (int j = 0; j < NTASKS_PER; j++) {
            char name[16];
            snprintf(name, sizeof(name), "t%d", j);
            if (aegis_task_create(&tasks[j], name, NULL) != AEGIS_OK) {
                __sync_fetch_and_or(&g_failures, 1);
                aegis_task_graph_destroy(g);
                goto next;
            }
            if (aegis_task_graph_add_task(g, tasks[j]) != AEGIS_OK) {
                __sync_fetch_and_or(&g_failures, 1);
                aegis_task_graph_destroy(g);
                goto next;
            }
            /* Record ID for duplicate detection */
            uint32_t id = aegis_task_id(tasks[j]);
            pthread_mutex_lock(&g_ids_lock);
            if (g_id_count >= g_id_cap) {
                g_id_cap = g_id_cap ? g_id_cap * 2 : 64;
                uint32_t* tmp = realloc(g_all_ids, sizeof(*g_all_ids) * g_id_cap);
                assert(tmp);
                g_all_ids = tmp;
            }
            g_all_ids[g_id_count++] = id;
            pthread_mutex_unlock(&g_ids_lock);
        }

        /* Build: T0→T1→T2, T0→T3, T2+T3→T4 */
        if (aegis_task_graph_add_dependency(g, tasks[0], tasks[1]) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);
        if (aegis_task_graph_add_dependency(g, tasks[1], tasks[2]) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);
        if (aegis_task_graph_add_dependency(g, tasks[0], tasks[3]) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);
        if (aegis_task_graph_add_dependency(g, tasks[2], tasks[4]) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);
        if (aegis_task_graph_add_dependency(g, tasks[3], tasks[4]) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);

        if (!aegis_task_graph_is_dag(g))
            __sync_fetch_and_or(&g_failures, 1);
        if (aegis_task_graph_validate(g) != AEGIS_OK)
            __sync_fetch_and_or(&g_failures, 1);

        aegis_task_t** ready = NULL;
        size_t cnt = 0;
        aegis_task_graph_ready_tasks(g, &ready, &cnt);
        free(ready);

        /* Proper cleanup: remove and destroy each task, then destroy graph */
        for (int j = 0; j < NTASKS_PER; j++) {
            aegis_task_graph_remove_task(g, tasks[j]);
            aegis_task_destroy(tasks[j]);
        }
        aegis_task_graph_destroy(g);

    next:
        ;
    }
    free(a);
    return NULL;
}

int main(void) {
    const size_t total_expected = (size_t)NTHREADS * NITER * NTASKS_PER;

    g_all_ids = malloc(sizeof(*g_all_ids) * total_expected);
    assert(g_all_ids);

    pthread_t threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        arg_t* a = malloc(sizeof(*a));
        assert(a);
        a->id = i;
        assert(pthread_create(&threads[i], NULL, worker, a) == 0);
    }
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(threads[i], NULL);

    /* Check for duplicate IDs — any duplicate means the race still exists */
    int duplicates = 0;
    for (size_t i = 0; i < g_id_count && !duplicates; i++) {
        for (size_t j = i + 1; j < g_id_count && !duplicates; j++) {
            if (g_all_ids[i] == g_all_ids[j]) {
                printf("DUPLICATE ID %u at indices [%zu, %zu]\n",
                       g_all_ids[i], i, j);
                duplicates++;
            }
        }
    }

    free(g_all_ids);

    if (g_failures > 0 || duplicates > 0) {
        fprintf(stderr, "REGRESSION: %d dup IDs, %d func failures\n",
                duplicates, g_failures);
        return 1;
    }

    printf("task graph concurrency regression test passed (%zu IDs unique)\n",
           g_id_count);
    return 0;
}
