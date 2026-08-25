#ifndef AEGIS_SCHEDULER_H
#define AEGIS_SCHEDULER_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file scheduler.h
 * @brief Opaque scheduler handle.
 *
 * Scheduler determines which ready tasks execute and in what order.
 * See aegis_scheduler_create / aegis_scheduler_destroy for lifecycle.
 */

typedef struct aegis_scheduler aegis_scheduler_t;

/**
 * @brief Create a new scheduler.
 *
 * @param[out] out  Receives the scheduler handle. Ownership: transferred.
 * @return AEGIS_OK on success, or an error code.
 */
aegis_status_t aegis_scheduler_create(aegis_scheduler_t** out);

/**
 * @brief Destroy a scheduler and release all resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param sched Handle to destroy. After return, pointer is invalid. Ownership: consumed.
 */
void aegis_scheduler_destroy(aegis_scheduler_t* sched);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SCHEDULER_H */
