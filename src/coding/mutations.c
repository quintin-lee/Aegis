#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/mutations.h"
#include <stdlib.h>
#include <pthread.h>

struct aegis_mutation_queue {
    pthread_mutex_t lock;
};

aegis_status_t aegis_mutation_queue_create(aegis_mutation_queue_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_mutation_queue_t* q = (aegis_mutation_queue_t*)calloc(1, sizeof(*q));
    if (!q) {
        return AEGIS_ERR_NOMEM;
    }
    pthread_mutex_init(&q->lock, NULL);
    *out = q;
    return AEGIS_OK;
}

void aegis_mutation_queue_destroy(aegis_mutation_queue_t* q)
{
    if (!q) {
        return;
    }
    pthread_mutex_destroy(&q->lock);
    free(q);
}

aegis_status_t aegis_mutation_queue_acquire(aegis_mutation_queue_t* q, const char* path)
{
    (void)path;
    if (!q) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&q->lock);
    return AEGIS_OK;
}

void aegis_mutation_queue_release(aegis_mutation_queue_t* q, const char* path)
{
    (void)path;
    if (!q) {
        return;
    }
    pthread_mutex_unlock(&q->lock);
}
