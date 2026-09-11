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

/**
 * @brief Global task ID counter (starts at 1; 0 is reserved as "no task").
 *
 * Atomic so tasks created concurrently from multiple threads never collide.
 */
static _Atomic uint32_t g_next_task_id = 1;

/**
 * @brief Allocate the next unique task ID.
 *
 * @return Fresh ID (monotonically increasing; wraps only after 4G tasks).
 */
static uint32_t next_task_id(void)
{
    return atomic_fetch_add(&g_next_task_id, 1);
}

/**
 * @brief Create a task with a unique ID, PENDING state, and default settings.
 *
 * Name/description are truncated to fit their fixed buffers (always NUL-
 * terminated). The retry policy is zeroed, timeout is 0 (none), and no
 * input/output data is attached.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param name Non-empty task name (borrowed; copied).
 * @param desc Optional description (borrowed; may be NULL).
 * @return AEGIS_OK, AEGIS_ERR_INVALID on empty name, AEGIS_ERR_NOMEM on failure.
 */
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

/**
 * @brief Destroy a task plus its input/output buffers and lock.
 *
 * Safe to call with NULL (no-op).
 *
 * @param task Handle to destroy (ownership: consumed).
 */
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

/**
 * @brief Return the unique task ID (0 for NULL input).
 *
 * @param task Handle (borrowed).
 * @return Task ID.
 */
uint32_t aegis_task_id(const aegis_task_t* task)
{
    return task ? task->id : 0;
}

/**
 * @brief Borrow the task name (NULL for NULL input).
 *
 * @param task Handle (borrowed).
 * @return Borrowed name string, owned by the task.
 */
const char* aegis_task_name(const aegis_task_t* task)
{
    if (!task) {
        return NULL;
    }
    return task->name;
}

/**
 * @brief Borrow the description (NULL when empty or NULL input).
 *
 * @param task Handle (borrowed).
 * @return Borrowed description, or NULL.
 */
const char* aegis_task_description(const aegis_task_t* task)
{
    if (!task) {
        return NULL;
    }
    return task->description[0] ? task->description : NULL;
}

/**
 * @brief Read the task type (CUSTOM for NULL input).
 *
 * @param task Handle (borrowed).
 * @return Task type tag.
 */
aegis_task_type_t aegis_task_type(const aegis_task_t* task)
{
    return task ? task->type : AEGIS_TASK_TYPE_CUSTOM;
}

/**
 * @brief Set the task type (thread-safe; no-op for NULL input).
 *
 * @param task Handle (borrowed).
 * @param type New type tag.
 */
void aegis_task_set_type(aegis_task_t* task, aegis_task_type_t type)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->type = type;
    aegis_mutex_unlock(task->lock);
}

/**
 * @brief Read the scheduling priority under the task lock (0 for NULL).
 *
 * @param task Handle (borrowed).
 * @return Priority value.
 */
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

/**
 * @brief Set the scheduling priority (thread-safe; no-op for NULL input).
 *
 * @param task Handle (borrowed).
 * @param priority New priority value.
 */
void aegis_task_set_priority(aegis_task_t* task, int priority)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->priority = priority;
    aegis_mutex_unlock(task->lock);
}

/**
 * @brief Read the lifecycle state under the task lock (PENDING for NULL).
 *
 * @param task Handle (borrowed).
 * @return Current state.
 */
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

/**
 * @brief Borrow the failure message (NULL when empty or NULL input).
 *
 * The returned pointer is owned by the task and valid only until the
 * next aegis_task_set_error call; it must not be freed.
 *
 * @param task Handle (borrowed).
 * @return Borrowed message, or NULL.
 */
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

/**
 * @brief Replace the input payload with a copy of @p data (thread-safe).
 *
 * Reallocates the input buffer to exactly @p size bytes; a zero size frees
 * the buffer. Rejected when @p size exceeds AEGIS_TASK_DATA_MAX.
 *
 * @param task Handle (borrowed).
 * @param data Bytes to copy (borrowed; NULL with size 0 clears).
 * @param size Bytes to store.
 * @return AEGIS_OK, AEGIS_ERR_INVALID on NULL task / oversize, AEGIS_ERR_NOMEM on failure.
 */
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

/**
 * @brief Borrow the input payload and report its size (thread-safe).
 *
 * The pointer stays owned by the task; a later set_input may invalidate it.
 *
 * @param task Handle (borrowed).
 * @param[out] out_size Receives the payload size (0 for NULL task; may be NULL).
 * @return Borrowed input bytes, or NULL when empty / NULL task.
 */
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

/**
 * @brief Replace the output payload with a copy of @p data (thread-safe).
 *
 * Same reallocation semantics as aegis_task_set_input; limited to
 * AEGIS_TASK_DATA_MAX bytes.
 *
 * @param task Handle (borrowed).
 * @param data Bytes to copy (borrowed; NULL with size 0 clears).
 * @param size Bytes to store.
 * @return AEGIS_OK, AEGIS_ERR_INVALID on NULL task / oversize, AEGIS_ERR_NOMEM on failure.
 */
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

/**
 * @brief Borrow the output payload and report its size (thread-safe).
 *
 * @param task Handle (borrowed).
 * @param[out] out_size Receives the payload size (0 for NULL task; may be NULL).
 * @return Borrowed output bytes, or NULL when empty / NULL task.
 */
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

/**
 * @brief Snapshot the retry policy by value (thread-safe; zeroed for NULL).
 *
 * @param task Handle (borrowed).
 * @return Copy of the retry policy.
 */
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

/**
 * @brief Replace the retry policy (thread-safe; no-op for NULL input).
 *
 * @param task   Handle (borrowed).
 * @param policy New policy (copied by value).
 */
void aegis_task_set_retry_policy(aegis_task_t* task, aegis_task_retry_policy_t policy)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->retry_policy = policy;
    aegis_mutex_unlock(task->lock);
}

/**
 * @brief Read the timeout in milliseconds (0 = none; 0 for NULL input).
 *
 * @param task Handle (borrowed).
 * @return Timeout in ms.
 */
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

/**
 * @brief Set the timeout in milliseconds (0 = none; no-op for NULL input).
 *
 * @param task       Handle (borrowed).
 * @param timeout_ms New timeout in ms.
 */
void aegis_task_set_timeout_ms(aegis_task_t* task, long timeout_ms)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->timeout_ms = timeout_ms;
    aegis_mutex_unlock(task->lock);
}

/**
 * @brief Insert, update, or remove a metadata entry (thread-safe).
 *
 * An existing key is updated in place; a NULL @p value removes the entry
 * (shifting the tail). New keys append until AEGIS_TASK_METADATA_MAX.
 * Keys/values are truncated to fit their fixed buffers.
 *
 * @param task  Handle (borrowed).
 * @param key   Entry key (borrowed; must be non-NULL).
 * @param value New value (borrowed; NULL removes the entry).
 * @return AEGIS_OK, AEGIS_ERR_INVALID on NULL task/key, AEGIS_ERR_BUSY when full.
 */
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

/**
 * @brief Borrow a metadata value by key (thread-safe; NULL on miss/empty).
 *
 * The pointer is owned by the task and valid only until the entry is
 * modified or removed.
 *
 * @param task Handle (borrowed).
 * @param key  Entry key (borrowed).
 * @return Borrowed value, or NULL.
 */
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

/**
 * @brief Remove a metadata entry (no-op when missing; NULL args rejected).
 *
 * Thin wrapper over aegis_task_set_metadata(task, key, NULL).
 *
 * @param task Handle (borrowed).
 * @param key  Entry key (borrowed).
 */
void aegis_task_remove_metadata(aegis_task_t* task, const char* key)
{
    if (!task || !key) {
        return;
    }
    aegis_task_set_metadata(task, key, NULL);
}

/**
 * @brief Force the lifecycle state (thread-safe; no-op for NULL input).
 *
 * Unlike aegis_task_try_begin_execution this performs no transition check —
 * it is the low-level primitive used by executors and tests.
 *
 * @param task  Handle (borrowed).
 * @param state New state.
 */
void aegis_task_set_state(aegis_task_t* task, aegis_task_state_t state)
{
    if (!task) {
        return;
    }
    aegis_mutex_lock(task->lock);
    task->state = state;
    aegis_mutex_unlock(task->lock);
}

/**
 * @brief Test-only alias for aegis_task_set_state.
 *
 * Exists so unit tests can force states without linking executor internals.
 *
 * @param task  Handle (borrowed).
 * @param state New state.
 */
void aegis_task_set_state_for_test(aegis_task_t* task, aegis_task_state_t state)
{
    aegis_task_set_state(task, state);
}

/**
 * @brief Atomically claim a task for execution (PENDING/READY → RUNNING).
 *
 * The check-and-set runs under the task lock, so concurrent executors race
 * safely and exactly one wins. Tasks in any other state are left untouched.
 *
 * @param task Handle (borrowed).
 * @return true when the transition happened (caller owns execution), false otherwise.
 */
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

/**
 * @brief Record (or clear, with NULL) the failure message (thread-safe).
 *
 * The message is truncated to fit the fixed buffer (always NUL-terminated).
 *
 * @param task    Handle (borrowed).
 * @param message Message to store (borrowed; NULL clears).
 */
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
