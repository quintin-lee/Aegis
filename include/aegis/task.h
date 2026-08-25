#ifndef AEGIS_TASK_H
#define AEGIS_TASK_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file task.h
 * @brief Task state enum and opaque task handle.
 */

/**
 * @brief Task execution states.
 *
 * Transition: PENDING → READY → RUNNING → DONE / FAILED / CANCELLED
 */
typedef enum aegis_task_state {
    AEGIS_TASK_PENDING,   /**< Scheduled but not yet ready to run.          */
    AEGIS_TASK_READY,     /**< Dependencies satisfied; eligible for scheduling. */
    AEGIS_TASK_RUNNING,   /**< Currently being executed by an executor.         */
    AEGIS_TASK_DONE,      /**< Completed successfully.                          */
    AEGIS_TASK_FAILED,    /**< Execution failed after retries exhausted.        */
    AEGIS_TASK_CANCELLED, /**< Cancelled by caller before completion.           */
} aegis_task_state_t;

/** Opaque task handle. */
typedef struct aegis_task aegis_task_t;

/**
 * @brief Create a new task with the given description.
 *
 * @param[out] out     Receives the task handle. Ownership: transferred.
 * @param[in]  desc    Task description string (borrowed; must remain valid during task lifetime).
 * @return AEGIS_OK on success, or an error code.
 */
aegis_status_t aegis_task_create(aegis_task_t** out, const char* desc);

/**
 * @brief Destroy a task and release all resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param task Handle to destroy. After return, pointer is invalid. Ownership: consumed.
 */
void aegis_task_destroy(aegis_task_t* task);

/**
 * @brief Get the current execution state of a task.
 *
 * @param task Task handle (borrowed).
 * @return Current task state.
 */
aegis_task_state_t aegis_task_state(const aegis_task_t* task);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TASK_H */
