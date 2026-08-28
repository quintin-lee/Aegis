/**
 * @file test_memory_concurrent.c
 * @brief Concurrent memory access boundary test.
 *
 * Verifies that external locking provides correct concurrent access to
 * memory stores (which are documented as NOT thread-safe internally).
 */
#include "aegis/memory/memory.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRESS_THREADS 4
#define STRESS_ITEMS   256

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void* stress_insert(void* arg)
{
    aegis_memory_t* mem = (aegis_memory_t*)arg;
    for (int i = 0; i < STRESS_ITEMS; i++) {
        char id[64];
        snprintf(id, sizeof(id), "idx_%d_%d", (int)pthread_self() & 0xFFFF, i);
        aegis_memory_item_t* item = calloc(1, sizeof(*item));
        if (!item) return NULL;
        item->id         = strdup(id);
        item->content    = strdup("concurrent_data");
        item->type       = AEGIS_MEMORY_ITEM_GENERIC;
        item->priority   = i;
        item->timestamp  = 0;
        if (!item->id || !item->content) {
            free(item->id);
            free(item->content);
            free(item);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        aegis_status_t rc = aegis_memory_put(mem, item);
        pthread_mutex_unlock(&g_lock);
        /* aegis_memory_put clones and frees the original — caller must NOT free. */
        if (rc != AEGIS_OK) {
            free(item->id);
            free(item->content);
            free(item);
        } else {
            item->id         = NULL;
            item->content    = NULL;
            free(item);
        }
    }
    return NULL;
}

static void test_concurrent_with_external_lock(void)
{
    printf("[test] concurrent_with_external_lock ...\n");
    aegis_memory_t* mem = NULL;
    assert(aegis_memory_create(&mem) == AEGIS_OK);

    pthread_t threads[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, stress_insert, mem) == 0);
    }
    for (int i = 0; i < STRESS_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    size_t count = aegis_memory_count(mem);
    assert(count == (size_t)(STRESS_THREADS * STRESS_ITEMS));
    printf("  locked count=%zu PASS\n", count);

    aegis_memory_destroy(mem);
}

int main(void)
{
    test_concurrent_with_external_lock();
    printf("test_memory_concurrent: all cases passed\n");
    return 0;
}
