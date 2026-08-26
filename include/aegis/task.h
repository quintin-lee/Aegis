/**
 * @file task.h
 * @brief Task definition, lifecycle, and state machine.
 *
 * A Task represents a unit of work within a Task Graph. It carries
 * metadata (name, description, type, priority), execution configuration
 * (retry policy, timeout), input/output data, and dependencies on other
 * tasks.
 *
 * State machine:
 *   PENDING → READY → RUNNING → SUCCESS / FAILED / CANCELLED / SKIPPED
 *                    ↑         ↓
 *                   WAITING ←────┘ (retry)
 *
 * All state transitions are guarded by the task's internal mutex.
 */
#ifndef AEGIS_TASK_H
#define AEGIS_TASK_H

#include "aegis/types.h"
#include "aegis/status.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task type classification.
 */
typedef enum aegis_task_type {
    AEGIS_TASK_TYPE_COMPUTATIONAL,   /**< Pure computation, no I/O.          */
    AEGIS_TASK_TYPE_IO,              /**< File or network I/O operation.      */
    AEGIS_TASK_TYPE_NETWORK,         /**< Network request (HTTP, RPC, etc.).  */
    AEGIS_TASK_TYPE_SHELL,           /**< Shell command execution.            */
    AEGIS_TASK_TYPE_TOOL,            /**< Tool invocation.                    */
    AEGIS_TASK_TYPE_PROVISION,       /**< Environment or resource provisioning.*/
    AEGIS_TASK_TYPE_SYNCHRONIZATION, /**< Wait point for multi-branch merge.  */
    AEGIS_TASK_TYPE_CUSTOM,          /**< User-defined task type.             */
} aegis_task_type_t;

/**
 * @brief Task execution states.
 *
 * Transition rules:
 *   PENDING  → READY      (dependencies satisfied)
 *   PENDING  → CANCELLED  (caller cancels before scheduling)
 *   PENDING  → SKIPPED    (predecessor failed, skip-on-fail)
 *   READY    → RUNNING    (executor picks up)
 *   READY    → CANCELLED  (caller cancels)
 *   READY    → SKIPPED    (predecessor failed, skip-on-fail)
 *   RUNNING  → SUCCESS    (execution completed)
 *   RUNNING  → FAILED     (execution failed, retries exhausted)
 *   RUNNING  → CANCELLED  (caller cancels)
 *   RUNNING  → WAITING    (retry scheduled)
 *   WAITING  → RUNNING    (retry begins)
 *   WAITING  → CANCELLED  (caller cancels during retry wait)
 *   SUCCESS  → (terminal)
 *   FAILED   → (terminal)
 *   CANCELLED→ (terminal)
 *   SKIPPED  → (terminal)
 */
typedef enum aegis_task_state {
    AEGIS_TASK_PENDING,   /**< Created, dependencies not yet satisfied. */
    AEGIS_TASK_READY,     /**< Dependencies met, awaiting executor.     */
    AEGIS_TASK_RUNNING,   /**< Currently executing.                     */
    AEGIS_TASK_WAITING,   /**< Paused for retry backoff.                */
    AEGIS_TASK_SUCCESS,   /**< Completed successfully.                  */
    AEGIS_TASK_FAILED,    /**< Failed after retries exhausted.          */
    AEGIS_TASK_CANCELLED, /**< Cancelled by caller.                     */
    AEGIS_TASK_SKIPPED,   /**< Skipped due to predecessor failure.      */
} aegis_task_state_t;

/**
 * @brief Retry policy for a task.
 */
typedef struct aegis_task_retry_policy {
    int  max_attempts;        /**< Maximum retry attempts (0 = no retries). */
    int  delay_ms;            /**< Delay between retries in milliseconds.   */
    bool exponential_backoff; /**< Use exponential backoff (default off).*/
} aegis_task_retry_policy_t;

/** Opaque task handle. */
typedef struct aegis_task aegis_task_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create a new task.
 *
 * @param[out] out       Receives the task handle. Ownership: transferred.
 * @param[in]  name      Task name (must be non-NULL, non-empty). Borrowed.
 * @param[in]  desc      Task description (may be NULL). Borrowed.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_task_create(aegis_task_t** out, const char* name, const char* desc);

/**
 * @brief Destroy a task and release all owned resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param task Handle to destroy (ownership: consumed).
 */
void aegis_task_destroy(aegis_task_t* task);

/* ── Identity ──────────────────────────────────────────────────────────────── */

/**
 * @brief Get the task's unique ID.
 *
 * @param task Task handle (borrowed).
 * @return Task ID (uint32_t).
 */
uint32_t aegis_task_id(const aegis_task_t* task);

/**
 * @brief Get the task name.
 *
 * @param task Task handle (borrowed).
 * @return Name string (NULL if not set). Borrowed.
 */
const char* aegis_task_name(const aegis_task_t* task);

/**
 * @brief Get the task description.
 *
 * @param task Task handle (borrowed).
 * @return Description string (NULL if not set). Borrowed.
 */
const char* aegis_task_description(const aegis_task_t* task);

/* ── Properties ────────────────────────────────────────────────────────────── */

/**
 * @brief Get the task type.
 *
 * @param task Task handle (borrowed).
 * @return Task type.
 */
aegis_task_type_t aegis_task_type(const aegis_task_t* task);

/**
 * @brief Set the task type.
 *
 * @param task Task handle.
 * @param type New task type.
 */
void aegis_task_set_type(aegis_task_t* task, aegis_task_type_t type);

/**
 * @brief Get the task priority (higher = more important).
 *
 * @param task Task handle (borrowed).
 * @return Priority value.
 */
int aegis_task_priority(const aegis_task_t* task);

/**
 * @brief Set the task priority.
 *
 * @param task    Task handle.
 * @param priority Priority value (higher = more important).
 */
void aegis_task_set_priority(aegis_task_t* task, int priority);

/* ── State ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Get the current task state.
 *
 * Thread-safe.
 *
 * @param task Task handle (borrowed).
 * @return Current state.
 */
aegis_task_state_t aegis_task_state(const aegis_task_t* task);

/**
 * @brief Get the task error message (if FAILED).
 *
 * @param task Task handle (borrowed).
 * @return Error message string (NULL if none). Borrowed.
 */
const char* aegis_task_error(const aegis_task_t* task);

/* ── Data ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Set task input data.
 *
 * The data is copied internally. Caller retains ownership of the
 * original buffer.
 *
 * @param task     Task handle.
 * @param data     Input data bytes.
 * @param size     Input data size in bytes.
 * @return AEGIS_OK on success, or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_task_set_input(aegis_task_t* task, const void* data, size_t size);

/**
 * @brief Get task input data.
 *
 * @param task Task handle (borrowed).
 * @param[out] out_size  Receives input size in bytes (may be NULL).
 * @return Pointer to input data (borrowed, do not free).
 */
const void* aegis_task_input(const aegis_task_t* task, size_t* out_size);

/**
 * @brief Set task output data.
 *
 * @param task     Task handle.
 * @param data     Output data bytes.
 * @param size     Output data size in bytes.
 * @return AEGIS_OK on success, or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_task_set_output(aegis_task_t* task, const void* data, size_t size);

/**
 * @brief Get task output data.
 *
 * @param task Task handle (borrowed).
 * @param[out] out_size  Receives output size in bytes (may be NULL).
 * @return Pointer to output data (borrowed, do not free).
 */
const void* aegis_task_output(const aegis_task_t* task, size_t* out_size);

/* ── Retry Policy ──────────────────────────────────────────────────────────── */

/**
 * @brief Get the retry policy.
 *
 * @param task Task handle (borrowed).
 * @return Retry policy (read-only copy).
 */
aegis_task_retry_policy_t aegis_task_retry_policy(const aegis_task_t* task);

/**
 * @brief Set the retry policy.
 *
 * @param task       Task handle.
 * @param policy     Retry policy to apply.
 */
void aegis_task_set_retry_policy(aegis_task_t* task, aegis_task_retry_policy_t policy);

/* ── Timeout ───────────────────────────────────────────────────────────────── */

/**
 * @brief Get the task timeout in milliseconds.
 *
 * @param task Task handle (borrowed).
 * @return Timeout in ms (0 = no timeout).
 */
long aegis_task_timeout_ms(const aegis_task_t* task);

/**
 * @brief Set the task timeout in milliseconds.
 *
 * @param task       Task handle.
 * @param timeout_ms Timeout duration (0 = no timeout).
 */
void aegis_task_set_timeout_ms(aegis_task_t* task, long timeout_ms);

/* ── Metadata ──────────────────────────────────────────────────────────────── */

/**
 * @brief Set a metadata key-value pair.
 *
 * @param task    Task handle.
 * @param key     Metadata key (borrowed, must not be NULL).
 * @param value   Metadata value (borrowed, may be NULL to remove).
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_task_set_metadata(aegis_task_t* task, const char* key, const char* value);

/**
 * @brief Get a metadata value.
 *
 * @param task Task handle (borrowed).
 * @param key  Metadata key (borrowed, must not be NULL).
 * @return Value string (NULL if key not found). Borrowed.
 */
const char* aegis_task_get_metadata(const aegis_task_t* task, const char* key);

/**
 * @brief Remove a metadata key.
 *
 * @param task Task handle.
 * @param key  Metadata key to remove (borrowed).
 */
void aegis_task_remove_metadata(aegis_task_t* task, const char* key);

#ifdef __cplusplus
}
#endif

/**
 * @brief Set the task state (test-only; bypasses validation).
 *
 * For testing purposes only. In production, use scheduler/executor
 * to drive state transitions.
 */
void aegis_task_set_state_for_test(aegis_task_t* task, aegis_task_state_t state);

#endif /* AEGIS_TASK_H */
