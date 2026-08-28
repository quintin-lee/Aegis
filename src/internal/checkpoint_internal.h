/**
 * @file checkpoint_internal.h
 * @brief Internal layout for the checkpoint module.
 *
 * NOT part of the public ABI.
 */
#ifndef AEGIS_CHECKPOINT_INTERNAL_H
#define AEGIS_CHECKPOINT_INTERNAL_H

#include "aegis/checkpoint/checkpoint.h"

#include <stdint.h>
#include <stddef.h>

/** Maximum number of tasks in a checkpoint. */
#define AEGIS_CHECKPOINT_MAX_TASKS 256u

/** Checksum algorithm: CRC32. */
#define AEGIS_CHECKPOINT_CRC32_INIT 0u

/* ── Checkpoint internals ──────────────────────────────────────────────────── */

struct aegis_checkpoint {
    uint32_t  version;           /**< Monotonic checkpoint sequence.        */
    uint64_t  timestamp;         /**< Seconds since epoch.                 */
    char      agent_state[32];   /**< Agent state name (e.g. "RUNNING").   */
    char*     goal;              /**< Owned copy of goal text.             */
    char*     plan_text;         /**< Owned serialized plan DSL.           */
    uint32_t  plan_version;      /**< Plan version at checkpoint time.     */
    uint64_t  iteration;         /**< Agent runtime iteration at snapshot. */
    size_t    n_tasks;           /**< Number of task snapshots.            */
    aegis_checkpoint_task_snapshot_t
              tasks[AEGIS_CHECKPOINT_MAX_TASKS]; /**< Fixed-size array.    */
};

#endif /* AEGIS_CHECKPOINT_INTERNAL_H */
