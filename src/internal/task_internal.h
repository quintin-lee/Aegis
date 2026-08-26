/**
 * @file task_internal.h
 * @brief Internal task struct layout.
 *
 * NOT part of the public API. Included by task.c only.
 */
#ifndef AEGIS_TASK_INTERNAL_H
#define AEGIS_TASK_INTERNAL_H

#include "aegis/task.h"
#include "aegis/common/mutex.h"
#include <stdint.h>
#include <stdbool.h>

/** Maximum name length. */
#define AEGIS_TASK_NAME_MAX 256
/** Maximum description length. */
#define AEGIS_TASK_DESC_MAX 1024
/** Maximum metadata entries. */
#define AEGIS_TASK_METADATA_MAX 32
/** Maximum metadata key length. */
#define AEGIS_TASK_META_KEY_MAX 64
/** Maximum metadata value length. */
#define AEGIS_TASK_META_VALUE_MAX 512
/** Maximum input/output data size. */
#define AEGIS_TASK_DATA_MAX (1024 * 1024) /* 1 MB */

/** Internal task metadata entry. */
typedef struct aegis_task_metadata {
    char key[AEGIS_TASK_META_KEY_MAX];
    char value[AEGIS_TASK_META_VALUE_MAX];
} aegis_task_metadata_t;

/** Internal task structure. */
struct aegis_task {
    /* Identity */
    uint32_t           id;
    char               name[AEGIS_TASK_NAME_MAX];
    char               description[AEGIS_TASK_DESC_MAX];

    /* Properties */
    aegis_task_type_t  type;
    int                priority;
    aegis_task_state_t state;

    /* Retry policy */
    aegis_task_retry_policy_t retry_policy;

    /* Timeout */
    long timeout_ms;

    /* Data */
    void*    input_data;
    size_t   input_size;
    void*    output_data;
    size_t   output_size;

    /* Error message */
    char     error_msg[256];

    /* Metadata */
    aegis_task_metadata_t metadata[AEGIS_TASK_METADATA_MAX];
    size_t                    n_metadata;

    /* Concurrency */
    aegis_mutex_t* lock;
};

/**
 * @brief Set the task state (thread-safe).
 *
 * Internal production setter for runtime components that drive the
 * documented state machine (e.g. scheduler promotion PENDING → READY).
 * Callers are responsible for transition validity.
 *
 * @param task  Task handle (borrowed; NULL is a no-op).
 * @param state New state.
 */
void aegis_task_set_state(aegis_task_t* task, aegis_task_state_t state);

#endif /* AEGIS_TASK_INTERNAL_H */
