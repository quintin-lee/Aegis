/**
 * @file runtime_internal.h
 * @brief Internal runtime state layout.
 *
 * This header is NOT part of the public API. It is included by runtime.c
 * only to define the opaque struct fields.
 */
#ifndef AEGIS_RUNTIME_INTERNAL_H
#define AEGIS_RUNTIME_INTERNAL_H

#include "aegis/runtime/runtime.h"
#include "aegis/common/thread.h"
#include "aegis/common/mutex.h"
#include "aegis/common/atomic.h"

/** Internal runtime state machine. */
typedef enum aegis_runtime_state {
    AEGIS_RT_CREATED,  /**< Handle allocated, not yet started. */
    AEGIS_RT_STARTING, /**< Worker threads being created. */
    AEGIS_RT_STARTED,  /**< All workers running. */
    AEGIS_RT_STOPPING, /**< Shutdown requested, joining threads. */
    AEGIS_RT_STOPPED,  /**< All threads joined. */
} aegis_runtime_state_t;

/** Internal runtime structure. */
struct aegis_runtime {
    /* Configuration */
    int         max_workers;
    size_t      event_queue_cap;
    long        stop_timeout_ms;
    const char* name;

    /* State */
    aegis_runtime_state_t state;
    aegis_atomic_int_t*   stop_requested;

    /* Worker threads */
    aegis_thread_t** worker_threads;
    size_t           n_workers;

    /* Concurrency primitive for state transitions */
    aegis_mutex_t* lock;

    /* Future extension points (currently NULL stubs) */
    void* event_loop;
    void* executor;
    void* planner;
    void* memory;
};

#endif /* AEGIS_RUNTIME_INTERNAL_H */
