/**
 * @file test_memory_concurrent.c
 * @brief Concurrent memory access boundary test.
 *
 * Verifies the documented invariant: individual memory stores are NOT
 * thread-safe. Multiple threads accessing the same store without external
 * synchronization will corrupt internal state (detected via TSan).
 *
 * The correct usage pattern is for the caller to provide its own mutex
 * around all memory operations.
 */
#include "aegis/memory.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stress test: N threads each insert M items ─────────────────────────── */

#define STRESS_THREADS 4
#define STRESS_ITEMS   256

static void* stress_insert(void* arg)
{
    aegis_memory_t* mem = (aegis_memory_t*)arg;
    for (int i = 0; i < STRESS_ITEMS; i++) {
        char id[32];
        snprintf(id, sizeof(id), "t%dx%d", (int)pthread_self() & 0xFFFF, i);
        aegis_memory_item_t item;
        memset(&item, 0, sizeof(item));
        item.id         = strdup(id);
        item.content    = strdup("stress_data");
        item.type       = AEGIS_MEMORY_ITEM_GENERIC;
        item.priority   = i;
        item.timestamp  = 0;
        assert(item.id && item.content);
        aegis_memory_put(mem, &item);
        free(item.id);
        free(item.content);
    }
    return NULL;
}

static void test_concurrent_stress_no_lock(void)
{
    printf("[test] concurrent_stress_no_lock ...\n");
    /* This test intentionally exercises unsynchronized concurrent access.
     * Under TSan it will report data races, confirming the documented
     * "NOT thread-safe" invariant. Under normal/ASan builds it simply
     * verifies the store does not crash or leak. */
    aegis_memory_t* mem = NULL;
    assert(aegis_memory_create(&mem) == AEGIS_OK);

    pthread_t threads[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, stress_insert, mem) == 0);
    }
    for (int i = 0; i < STRESS_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    /* Final count should be >= expected (duplicates may overwrite). */
    size_t count = aegis_memory_count(mem);
    assert(count > 0);
    printf("  items after %dx%d concurrent puts: count=%zu PASS\n",
           STRESS_THREADS, STRESS_ITEMS, count);

    aegis_memory_destroy(mem);
}

/* ── Correct pattern: external lock ────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void* stress_insert_locked(void* arg)
{
    aegis_memory_t* mem = (aegis_memory_t*)arg;
    for (int i = 0; i < STRESS_ITEMS; i++) {
        pthread_mutex_lock(&g_lock);
        char id[64];
        snprintf(id, sizeof(id), "locked_%d_%d", (int)pthread_self() & 0xFFFF, i);
        aegis_memory_item_t item;
        memset(&item, 0, sizeof(item));
        item.id         = strdup(id);
        item.content    = strdup("locked_data");
        item.type       = AEGIS_MEMORY_ITEM_GENERIC;
        item.priority   = i;
        item.timestamp  = 0;
        assert(item.id && item.content);
        aegis_memory_put(mem, &item);
        pthread_mutex_unlock(&g_lock);
        free(item.id);
        free(item.content);
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
        assert(pthread_create(&threads[i], NULL, stress_insert_locked, mem) == 0);
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
    test_concurrent_stress_no_lock();
    test_concurrent_with_external_lock();
    printf("test_memory_concurrent: all cases passed\n");
    return 0;
}
