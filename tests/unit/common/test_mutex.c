#include "aegis/common/thread.h"
#include "aegis/common/mutex.h"
#include <assert.h>
#include <stdio.h>

static int shared_counter = 0;

typedef struct {
    aegis_mutex_t* mutex;
    int            iterations;
} thread_arg_t;

static void* increment_thread(void* arg)
{
    thread_arg_t* ta = (thread_arg_t*)arg;
    for (int i = 0; i < ta->iterations; i++) {
        aegis_mutex_lock(ta->mutex);
        shared_counter++;
        aegis_mutex_unlock(ta->mutex);
    }
    return NULL;
}

int main(void)
{
    aegis_mutex_t* m = NULL;
    assert(aegis_mutex_create(&m, AEGIS_MUTEX_PLAIN) == 0);

    /* Try lock */
    assert(aegis_mutex_trylock(m) == true);
    assert(aegis_mutex_trylock(m) == false); /* already held */
    aegis_mutex_unlock(m);

    /* Scoped guard */
    aegis_mutex_guard_t g = aegis_mutex_guard_lock(m);
    assert(aegis_mutex_trylock(m) == false);
    aegis_mutex_t* released = aegis_mutex_guard_release(&g);
    assert(released == m);
    assert(g.mutex == NULL);
    aegis_mutex_unlock(m);

    /* Multi-threaded counter */
    shared_counter     = 0;
    aegis_thread_t *t1 = NULL, *t2 = NULL;
    thread_arg_t    arg1 = {m, 1000};
    thread_arg_t    arg2 = {m, 1000};
    assert(aegis_thread_create(&t1, increment_thread, &arg1, 0) == 0);
    assert(aegis_thread_create(&t2, increment_thread, &arg2, 0) == 0);
    aegis_thread_join(t1);
    aegis_thread_join(t2);
    aegis_thread_destroy(t1);
    aegis_thread_destroy(t2);
    assert(shared_counter == 2000);

    aegis_mutex_destroy(m);
    aegis_mutex_destroy(NULL);
    assert(aegis_mutex_create(NULL, AEGIS_MUTEX_PLAIN) != 0);

    return 0;
}
