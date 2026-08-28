/**
 * @file task.c
 * @brief Task lifecycle and property management.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/task/task.h"
#include "task_internal.h"
#include "lifecycle.h"
#include "aegis/common/mutex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/* Global task ID counter — atomic to avoid data races when tasks are
 * created concurrently from multiple threads. */
static _Atomic uint32_t g_next_task_id = 1;

static uint32_t next_task_id(void)
{
    return atomic_fetch_add(&g_next_task_id, 1);
}

aegis_status_t aegis_task_create(aegis_task_t** out, const char* name, const char* desc)
{
    AEGIS_CHECK_OUT(out);
    if (!name || name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }

    aegis_task_t* task = (aegis_task_t*)calloc(1, sizeof(*task));
    if (!task) {
        return AEGIS_ERR_NOMEM;
    }

    strncpy(task->name, name, sizeof(task->name) - 1);
    task->name[sizeof(task->name) - 1] = '\0';

    if (desc) {
        strncpy(task->description, desc, sizeof(task->description) - 1);
        task->description[sizeof(task->description) - 1] = '\0';
    } else {
        task->description[0] = '\0';
    }

    task->id                               = next_task_id();
    task->type                             = AEGIS_TASK_TYPE_CUSTOM;
    task->priority                         = 0;
    task->state                            = AEGIS_TASK_PENDING;
    task->retry_policy.max_attempts        = 0;
    task->retry_policy.delay_ms            = 0;
    task->retry_policy.exponential_backoff = false;
    task->timeout_ms                       = 0;
    task->input_data                       = NULL;
    task->input_size                       = 0;
    task->output_data                      = NULL;
    task->output_size                      = 0;
    task->error_msg[0]                     = '\0';
    task->n_metadata                       = 0;

    int rc = aegis_mutex_create(&task->lock, AEGIS_MUTEX_RECURSIVE);
    if (rc != 0) {
        free(task);
        return AEGIS_ERR_NOMEM;
    }

    *out = task;
    return AEGIS_OK;
}

void aegis_task_destroy(aegis_task_t* task)
{
    if (!task) {
        return;
    }
    free(task->input_data);
    free(task->output_data);
    aegis_mutex_destroy(task->lock);
    free(task);
}

uint32_t aegis_task_id(const aegis_task_t* task)
{
    return task ? task->id : 0;
}

const char* aegis_task_name(const aegis_task_t* task)
{
    if (!task) {
        return NULL;
    }
    return task->name;
}

const char* aegis_task_description(const aegis_task_t* task)
{
    if (!task) {
        return NULL;
    }
    return task->description[0] ? task->description : NULL;
}

aegis_task_type_t aegis_task_type(const aegis_task_t* task)
{
    return task ? task->type : AEGIS_TASK_TYPE_CUSTOM;
}

void aegis_task_set_type(aegis_task_t* task, aegis_task_type_t type)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->type = type;
    aegis_mutex_unlock(task->lock);
}

int aegis_task_priority(const aegis_task_t* task)
{
    if (!task) {
        return 0;
    }
    aegis_mutex_lock(task->lock);
    int p = task->priority;
    aegis_mutex_unlock(task->lock);
    return p;
}

void aegis_task_set_priority(aegis_task_t* task, int priority)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->priority = priority;
    aegis_mutex_unlock(task->lock);
}

aegis_task_state_t aegis_task_state(const aegis_task_t* task)
{
    if (!task) {
        return AEGIS_TASK_PENDING;
    }
    aegis_mutex_lock(task->lock);
    aegis_task_state_t s = task->state;
    aegis_mutex_unlock(task->lock);
    return s;
}

const char* aegis_task_error(const aegis_task_t* task)
{
    if (!task) {
        return NULL;
    }
    aegis_mutex_lock(task->lock);
    const char* e = task->error_msg[0] ? task->error_msg : NULL;
    aegis_mutex_unlock(task->lock);
    return e;
}

aegis_status_t aegis_task_set_input(aegis_task_t* task, const void* data, size_t size)
{
    if (!task) {
        return AEGIS_ERR_INVALID;
    }
    if (size > AEGIS_TASK_DATA_MAX) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(task->lock);
    void* new_data = realloc(task->input_data, size);
    if (size > 0 && !new_data) {
        aegis_mutex_unlock(task->lock);
        return AEGIS_ERR_NOMEM;
    }
    if (size > 0 && data) {
        memcpy(new_data, data, size);
    }
    task->input_data = new_data;
    task->input_size = size;
    aegis_mutex_unlock(task->lock);
    return AEGIS_OK;
}

const void* aegis_task_input(const aegis_task_t* task, size_t* out_size)
{
    if (!task) {
        if (out_size) {
            *out_size = 0;
        }
        return NULL;
    }
    aegis_mutex_lock(task->lock);
    if (out_size) {
        *out_size = task->input_size;
    }
    const void* d = task->input_size > 0 ? task->input_data : NULL;
    aegis_mutex_unlock(task->lock);
    return d;
}

aegis_status_t aegis_task_set_output(aegis_task_t* task, const void* data, size_t size)
{
    if (!task) {
        return AEGIS_ERR_INVALID;
    }
    if (size > AEGIS_TASK_DATA_MAX) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(task->lock);
    void* new_data = realloc(task->output_data, size);
    if (size > 0 && !new_data) {
        aegis_mutex_unlock(task->lock);
        return AEGIS_ERR_NOMEM;
    }
    if (size > 0 && data) {
        memcpy(new_data, data, size);
    }
    task->output_data = new_data;
    task->output_size = size;
    aegis_mutex_unlock(task->lock);
    return AEGIS_OK;
}

const void* aegis_task_output(const aegis_task_t* task, size_t* out_size)
{
    if (!task) {
        if (out_size) {
            *out_size = 0;
        }
        return NULL;
    }
    aegis_mutex_lock(task->lock);
    if (out_size) {
        *out_size = task->output_size;
    }
    const void* d = task->output_size > 0 ? task->output_data : NULL;
    aegis_mutex_unlock(task->lock);
    return d;
}

aegis_task_retry_policy_t aegis_task_retry_policy(const aegis_task_t* task)
{
    if (!task) {
        aegis_task_retry_policy_t empty = {0, 0, false};
        return empty;
    }
    aegis_mutex_lock(task->lock);
    aegis_task_retry_policy_t p = task->retry_policy;
    aegis_mutex_unlock(task->lock);
    return p;
}

void aegis_task_set_retry_policy(aegis_task_t* task, aegis_task_retry_policy_t policy)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->retry_policy = policy;
    aegis_mutex_unlock(task->lock);
}

long aegis_task_timeout_ms(const aegis_task_t* task)
{
    if (!task) {
        return 0;
    }
    aegis_mutex_lock(task->lock);
    long t = task->timeout_ms;
    aegis_mutex_unlock(task->lock);
    return t;
}

void aegis_task_set_timeout_ms(aegis_task_t* task, long timeout_ms)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->timeout_ms = timeout_ms;
    aegis_mutex_unlock(task->lock);
}

aegis_status_t aegis_task_set_metadata(aegis_task_t* task, const char* key, const char* value)
{
    if (!task || !key) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(task->lock);

    /* Update existing key */
    for (size_t i = 0; i < task->n_metadata; i++) {
        if (strcmp(task->metadata[i].key, key) == 0) {
            if (value) {
                strncpy(task->metadata[i].value, value, sizeof(task->metadata[i].value) - 1);
                task->metadata[i].value[sizeof(task->metadata[i].value) - 1] = '\0';
            } else {
                /* Remove: shift remaining entries */
                for (size_t j = i; j < task->n_metadata - 1; j++) {
                    task->metadata[j] = task->metadata[j + 1];
                }
                task->n_metadata--;
            }
            aegis_mutex_unlock(task->lock);
            return AEGIS_OK;
        }
    }

    /* Add new entry if capacity allows */
    if (task->n_metadata >= AEGIS_TASK_METADATA_MAX) {
        aegis_mutex_unlock(task->lock);
        return AEGIS_ERR_BUSY;
    }

    strncpy(task->metadata[task->n_metadata].key, key,
            sizeof(task->metadata[task->n_metadata].key) - 1);
    task->metadata[task->n_metadata].key[sizeof(task->metadata[task->n_metadata].key) - 1] = '\0';

    if (value) {
        strncpy(task->metadata[task->n_metadata].value, value,
                sizeof(task->metadata[task->n_metadata].value) - 1);
        task->metadata[task->n_metadata].value[sizeof(task->metadata[task->n_metadata].value) - 1] =
            '\0';
    } else {
        task->metadata[task->n_metadata].value[0] = '\0';
    }
    task->n_metadata++;

    aegis_mutex_unlock(task->lock);
    return AEGIS_OK;
}

const char* aegis_task_get_metadata(const aegis_task_t* task, const char* key)
{
    if (!task || !key) {
        return NULL;
    }

    aegis_mutex_lock(task->lock);
    for (size_t i = 0; i < task->n_metadata; i++) {
        if (strcmp(task->metadata[i].key, key) == 0) {
            const char* val = task->metadata[i].value[0] ? task->metadata[i].value : NULL;
            aegis_mutex_unlock(task->lock);
            return val;
        }
    }
    aegis_mutex_unlock(task->lock);
    return NULL;
}

void aegis_task_remove_metadata(aegis_task_t* task, const char* key)
{
    if (!task || !key) {
        return;
    }
    aegis_task_set_metadata(task, key, NULL);
}

void aegis_task_set_state(aegis_task_t* task, aegis_task_state_t state)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->state = state;
    aegis_mutex_unlock(task->lock);
}

void aegis_task_set_state_for_test(aegis_task_t* task, aegis_task_state_t state)
{
    aegis_task_set_state(task, state);
}

bool aegis_task_try_begin_execution(aegis_task_t* task)
{
    if (!task) {
        return false;
    }
    aegis_mutex_lock(task->lock);
    const bool submittable = (task->state == AEGIS_TASK_PENDING || task->state == AEGIS_TASK_READY);
    if (submittable) {
        task->state = AEGIS_TASK_RUNNING;
    }
    aegis_mutex_unlock(task->lock);
    return submittable;
}

void aegis_task_set_error(aegis_task_t* task, const char* message)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    if (message) {
        snprintf(task->error_msg, sizeof(task->error_msg), "%s", message);
    } else {
        task->error_msg[0] = '\0';
    }
    aegis_mutex_unlock(task->lock);
}
