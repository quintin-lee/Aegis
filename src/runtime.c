/**
 * @file runtime.c
 * @brief Runtime lifecycle implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/runtime.h"
#include "aegis/config.h"
#include "internal/runtime_internal.h"
#include <stdlib.h>
#include <string.h>

#define RT_DEFAULT_WORKERS    4
#define RT_DEFAULT_QUEUE_CAP  256
#define RT_DEFAULT_TIMEOUT_MS 5000L

aegis_config_t aegis_config_default(void)
{
    aegis_config_t c;
    c.max_workers     = RT_DEFAULT_WORKERS;
    c.event_queue_cap = RT_DEFAULT_QUEUE_CAP;
    c.stop_timeout_ms = RT_DEFAULT_TIMEOUT_MS;
    c.name            = NULL;
    return c;
}

aegis_status_t aegis_runtime_create(aegis_runtime_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_runtime_t* rt = (aegis_runtime_t*)calloc(1, sizeof(*rt));
    if (!rt) {
        return AEGIS_ERR_NOMEM;
    }

    /* Copy defaults */
    rt->max_workers     = RT_DEFAULT_WORKERS;
    rt->event_queue_cap = RT_DEFAULT_QUEUE_CAP;
    rt->stop_timeout_ms = RT_DEFAULT_TIMEOUT_MS;
    rt->name            = NULL;
    rt->state           = AEGIS_RT_CREATED;
    rt->n_workers       = 0;
    rt->worker_threads  = NULL;
    rt->event_loop      = NULL;
    rt->executor        = NULL;
    rt->planner         = NULL;
    rt->memory          = NULL;

    int rc = aegis_mutex_create(&rt->lock, AEGIS_MUTEX_RECURSIVE);
    if (rc != 0) {
        free(rt);
        return AEGIS_ERR_NOMEM;
    }

    rc = aegis_atomic_int_create(&rt->stop_requested, 0);
    if (rc != 0) {
        aegis_mutex_destroy(rt->lock);
        free(rt);
        return AEGIS_ERR_NOMEM;
    }

    *out = rt;
    return AEGIS_OK;
}

aegis_status_t aegis_runtime_start(aegis_runtime_t* rt)
{
    if (!rt) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(rt->lock);

    /* Idempotent: already started or starting → success */
    if (rt->state == AEGIS_RT_STARTED || rt->state == AEGIS_RT_STARTING) {
        aegis_mutex_unlock(rt->lock);
        return AEGIS_OK;
    }

    if (rt->state != AEGIS_RT_CREATED && rt->state != AEGIS_RT_STOPPED) {
        aegis_mutex_unlock(rt->lock);
        return AEGIS_ERR_INVALID;
    }

    /* Transition to STARTING */
    rt->state = AEGIS_RT_STARTING;
    aegis_atomic_int_store(rt->stop_requested, 0);

    /* Allocate worker thread handles */
    rt->worker_threads = (aegis_thread_t**)calloc((size_t)rt->max_workers, sizeof(aegis_thread_t*));
    if (!rt->worker_threads) {
        rt->state = AEGIS_RT_CREATED;
        aegis_mutex_unlock(rt->lock);
        return AEGIS_ERR_NOMEM;
    }

    aegis_mutex_unlock(rt->lock);

    /* Create and start worker threads — if any fail, tear down what we have */
    for (size_t i = 0; i < (size_t)rt->max_workers; i++) {
        aegis_status_t st =
            aegis_thread_create(&rt->worker_threads[i], NULL, /* no-op worker for now */
                                rt, 0);                       /* default stack */
        if (st != AEGIS_OK) {
            /* Teardown: destroy already-created threads */
            for (size_t j = 0; j < i; j++) {
                aegis_thread_join(rt->worker_threads[j]);
                aegis_thread_destroy(rt->worker_threads[j]);
            }
            free(rt->worker_threads);
            rt->worker_threads = NULL;
            aegis_mutex_lock(rt->lock);
            rt->state = AEGIS_RT_CREATED;
            aegis_mutex_unlock(rt->lock);
            return AEGIS_ERR_NOMEM;
        }
    }
    rt->n_workers = rt->max_workers;

    aegis_mutex_lock(rt->lock);
    rt->state = AEGIS_RT_STARTED;
    aegis_mutex_unlock(rt->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_runtime_stop(aegis_runtime_t* rt)
{
    if (!rt) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(rt->lock);

    /* Idempotent: already stopped or stopping → success */
    if (rt->state == AEGIS_RT_STOPPED || rt->state == AEGIS_RT_STOPPING) {
        aegis_mutex_unlock(rt->lock);
        return AEGIS_OK;
    }

    if (rt->state != AEGIS_RT_STARTED && rt->state != AEGIS_RT_STARTING) {
        aegis_mutex_unlock(rt->lock);
        return AEGIS_ERR_INVALID;
    }

    /* Transition to STOPPING and signal shutdown */
    rt->state = AEGIS_RT_STOPPING;
    aegis_atomic_int_store(rt->stop_requested, 1);
    aegis_mutex_unlock(rt->lock);

    /* Join all worker threads */
    for (size_t i = 0; i < rt->n_workers; i++) {
        aegis_thread_join(rt->worker_threads[i]);
        aegis_thread_destroy(rt->worker_threads[i]);
    }
    free(rt->worker_threads);
    rt->worker_threads = NULL;
    rt->n_workers      = 0;

    aegis_mutex_lock(rt->lock);
    rt->state = AEGIS_RT_STOPPED;
    aegis_mutex_unlock(rt->lock);
    return AEGIS_OK;
}

void aegis_runtime_destroy(aegis_runtime_t* rt)
{
    if (!rt) {
        return;
    }

    /* Stop if running */
    if (rt->state == AEGIS_RT_STARTED || rt->state == AEGIS_RT_STARTING) {
        aegis_runtime_stop(rt);
    }

    /* Clean up any remaining threads (STOPPING case where join already happened) */
    if (rt->worker_threads != NULL) {
        for (size_t i = 0; i < rt->n_workers; i++) {
            if (rt->worker_threads[i] != NULL) {
                aegis_thread_join(rt->worker_threads[i]);
                aegis_thread_destroy(rt->worker_threads[i]);
            }
        }
        free(rt->worker_threads);
    }

    aegis_atomic_int_destroy(rt->stop_requested);
    aegis_mutex_destroy(rt->lock);
    free(rt);
}
