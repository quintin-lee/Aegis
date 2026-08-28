#ifndef AEGIS_AUTONOMOUS_STATE_H
#define AEGIS_AUTONOMOUS_STATE_H

#include "aegis/status.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file autonomous_state.h
 * @brief Autonomous agent state machine definitions.
 *
 * States follow this hierarchy:
 *   CREATED -> INITIALIZING -> READY -> PLANNING -> SCHEDULING -> EXECUTING
 *                                                                  -> EVALUATING
 *                                                                  -> REFLECTING
 *                                                                  -> REPLANNING
 *                                                                  -> CHECKPOINTING
 *                                                                  -> RECOVERING
 *                       -> CANCELLING -> CANCELLED
 *                       -> FAILED
 *                       -> COMPLETED
 *
 * Transitions are guarded by mutex; illegal transitions return AEGIS_ERR_INVALID.
 * State changes publish events after releasing internal locks.
 */

/** Autonomous agent state enum. */
typedef enum aegis_autonomous_state {
    AEGIS_AUTO_CREATED = 0,   /**< Initial state after create. */
    AEGIS_AUTO_INITIALIZING,  /**< During create/initialization. */
    AEGIS_AUTO_READY,         /**< Ready to run. */
    AEGIS_AUTO_PLANNING,      /**< Generating plan from goal. */
    AEGIS_AUTO_SCHEDULING,    /**< Building task graph. */
    AEGIS_AUTO_EXECUTING,     /**< Running tasks in loop. */
    AEGIS_AUTO_EVALUATING,    /**< Critic evaluating results. */
    AEGIS_AUTO_REFLECTING,    /**< Reflection on execution. */
    AEGIS_AUTO_REPLANNING,    /**< Replanning based on reflection. */
    AEGIS_AUTO_CHECKPOINTING, /**< Saving checkpoint. */
    AEGIS_AUTO_RECOVERING,    /**< Restoring from checkpoint. */
    AEGIS_AUTO_COMPLETED,     /**< Successfully finished. */
    AEGIS_AUTO_FAILED,        /**< Failed irrecoverably. */
    AEGIS_AUTO_CANCELLING,    /**< Cancellation requested. */
    AEGIS_AUTO_CANCELLED,     /**< Cancelled successfully. */
} aegis_autonomous_state_t;

/** String representation of state. */
const char* aegis_autonomous_state_str(aegis_autonomous_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AUTONOMOUS_STATE_H */
