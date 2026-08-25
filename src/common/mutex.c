#define _POSIX_C_SOURCE 200809L
#include "aegis/common/mutex.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct aegis_mutex {
    pthread_mutex_t inner;
    int             recursive;
};

int aegis_mutex_create(aegis_mutex_t** out, aegis_mutex_kind_t kind)
{
    if (!out) {
        return -1;
    }
    aegis_mutex_t* m = calloc(1, sizeof(*m));
    if (!m) {
        return -1;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (kind == AEGIS_MUTEX_RECURSIVE) {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    }
    int rc = pthread_mutex_init(&m->inner, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        free(m);
        return -rc;
    }
    m->recursive = (kind == AEGIS_MUTEX_RECURSIVE) ? 1 : 0;
    *out         = m;
    return 0;
}

void aegis_mutex_destroy(aegis_mutex_t* m)
{
    if (!m) {
        return;
    }
    pthread_mutex_destroy(&m->inner);
    free(m);
}

void aegis_mutex_lock(aegis_mutex_t* m)
{
    pthread_mutex_lock(&m->inner);
}

bool aegis_mutex_trylock(aegis_mutex_t* m)
{
    int rc = pthread_mutex_trylock(&m->inner);
    return rc == 0;
}

void aegis_mutex_unlock(aegis_mutex_t* m)
{
    pthread_mutex_unlock(&m->inner);
}

aegis_mutex_guard_t aegis_mutex_guard_lock(aegis_mutex_t* m)
{
    aegis_mutex_guard_t g = {m};
    if (m) {
        pthread_mutex_lock(&m->inner);
    }
    return g;
}

aegis_mutex_t* aegis_mutex_guard_release(aegis_mutex_guard_t* g)
{
    if (!g) {
        return NULL;
    }
    aegis_mutex_t* m = g->mutex;
    g->mutex         = NULL;
    return m;
}
