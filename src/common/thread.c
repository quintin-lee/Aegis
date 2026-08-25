/**
 * @file thread.c
 * @brief POSIX thread wrapper with explicit lifecycle.
 *
 * Threads are created via aegis_thread_create and must be joined
 * before destruction. aegis_thread_join marks the thread as joined
 * and prevents double-join. aegis_thread_destroy must only be called
 * after join (or after the thread has exited independently).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/thread.h"
#include <pthread.h>
#include <stdlib.h>

struct aegis_thread {
    pthread_t handle;
    int       joined;
};

int aegis_thread_create(aegis_thread_t** out, aegis_thread_fn fn, void* arg, size_t stack_size)
{
    if (!out || !fn) {
        return -1;
    }
    aegis_thread_t* t = calloc(1, sizeof(*t));
    if (!t) {
        return -1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size > 0) {
        pthread_attr_setstacksize(&attr, stack_size);
    }
    int rc = pthread_create(&t->handle, &attr, (void* (*)(void*))fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return -rc;
    }
    *out = t;
    return 0;
}

void aegis_thread_join(aegis_thread_t* t)
{
    if (!t || t->joined) {
        return;
    }
    pthread_join(t->handle, NULL);
    t->joined = 1;
}

void aegis_thread_destroy(aegis_thread_t* t)
{
    if (!t) {
        return;
    }
    free(t);
}

void aegis_thread_yield(void)
{
    sched_yield();
}

uint64_t aegis_thread_id(void)
{
    return (uint64_t)pthread_self();
}
