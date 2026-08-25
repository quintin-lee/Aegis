#include "aegis/common/atomic.h"
#include "aegis/common/thread.h"
#include <assert.h>
#include <stdio.h>

#define NUM_THREADS    8
#define INC_PER_THREAD 100000
#define EXPECTED       (NUM_THREADS * INC_PER_THREAD)

typedef struct {
    aegis_atomic_int_t* counter;
    int                 iterations;
} thread_arg_t;

static void* inc_thread(void* arg)
{
    thread_arg_t* ta = (thread_arg_t*)arg;
    for (int i = 0; i < ta->iterations; i++) {
        aegis_atomic_int_inc(ta->counter);
    }
    return NULL;
}

static void* fetch_add_thread(void* arg)
{
    thread_arg_t* ta = (thread_arg_t*)arg;
    for (int i = 0; i < ta->iterations; i++) {
        int old = 0;
        aegis_atomic_int_fetch_add(ta->counter, 1, &old);
        (void)old;
    }
    return NULL;
}

int main(void)
{
    aegis_atomic_int_t* counter = NULL;

    /* --- create / destroy / NULL safety --- */
    assert(aegis_atomic_int_create(&counter, 0) == 0);
    assert(counter != NULL);
    aegis_atomic_int_destroy(NULL);
    aegis_atomic_int_destroy(counter);

    assert(aegis_atomic_int_create(NULL, 0) != 0);

    /* --- load / store --- */
    assert(aegis_atomic_int_create(&counter, 42) == 0);
    int val = 0;
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 42);
    aegis_atomic_int_store(counter, 7);
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 7);
    /* load into NULL out is allowed */
    assert(aegis_atomic_int_load(counter, NULL) == 0);
    aegis_atomic_int_destroy(counter);

    /* --- fetch_add --- */
    assert(aegis_atomic_int_create(&counter, 10) == 0);
    int old = 0;
    assert(aegis_atomic_int_fetch_add(counter, 5, &old) == 0);
    assert(old == 10);
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 15);
    /* fetch_add with NULL out */
    assert(aegis_atomic_int_fetch_add(counter, -3, NULL) == 0);
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 12);
    aegis_atomic_int_destroy(counter);

    /* --- compare_exchange --- */
    assert(aegis_atomic_int_create(&counter, 5) == 0);
    assert(aegis_atomic_int_compare_exchange(counter, 5, 10) == true);
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 10);
    assert(aegis_atomic_int_compare_exchange(counter, 5, 20) == false);
    assert(aegis_atomic_int_load(counter, &val) == 0);
    assert(val == 10);
    aegis_atomic_int_destroy(counter);

    /* --- stress test: concurrent increments --- */
    assert(aegis_atomic_int_create(&counter, 0) == 0);

    thread_arg_t    args[NUM_THREADS];
    aegis_thread_t* threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].counter    = counter;
        args[i].iterations = INC_PER_THREAD;
        assert(aegis_thread_create(&threads[i], inc_thread, &args[i], 0) == 0);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        aegis_thread_join(threads[i]);
        aegis_thread_destroy(threads[i]);
    }

    int result = 0;
    assert(aegis_atomic_int_load(counter, &result) == 0);
    printf("stress result: %d (expected %d)\n", result, EXPECTED);
    assert(result == EXPECTED);
    aegis_atomic_int_destroy(counter);

    /* --- stress test: concurrent fetch_add --- */
    assert(aegis_atomic_int_create(&counter, 0) == 0);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].counter    = counter;
        args[i].iterations = INC_PER_THREAD;
        assert(aegis_thread_create(&threads[i], fetch_add_thread, &args[i], 0) == 0);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        aegis_thread_join(threads[i]);
        aegis_thread_destroy(threads[i]);
    }

    result = 0;
    assert(aegis_atomic_int_load(counter, &result) == 0);
    printf("fetch_add stress result: %d (expected %d)\n", result, EXPECTED);
    assert(result == EXPECTED);
    aegis_atomic_int_destroy(counter);

    printf("all atomic tests passed\n");
    return 0;
}
