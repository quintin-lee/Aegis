/**
 * @file checkpoint.h
 * @brief Agent state persistence: save/restore with versioning and integrity.
 *
 * A Checkpoint captures a snapshot of the agent's complete execution state
 * at a point in time:
 *   - Agent state (state machine node)
 *   - Goal text
 *   - Serialized plan (STEP DSL)
 *   - Plan version
 *   - Task graph state (one entry per task: id, name, state, error, retry count)
 *
 * Design principles:
 *   - Versioned: each checkpoint has a monotonically increasing version.
 *   - Atomic write: content is written to a temp file, then rename()'d into
 *     place. Partial writes are detectable via the version/checksum header.
 *   - Integrity: a CRC32 checksum is stored in the header; restores verify it.
 *   - In-flight handling: tasks in RUNNING state are marked PENDING on
 *     recovery so they can be re-scheduled.
 *   - No circular dependency: checkpoint depends only on public APIs
 *     (agent, plan, graph, task). It does NOT depend on the storage
 *     provider layer — the caller supplies the write path.
 *
 * Thread safety: the checkpoint handle is single-threaded. Callers who
 * share a handle across threads must synchronize externally.
 */
#ifndef AEGIS_CHECKPOINT_H
#define AEGIS_CHECKPOINT_H

#include "aegis/cancellation.h"
#include "aegis/status.h"
#include "aegis/types.h"
#include "aegis/agent.h"
#include "aegis/task.h"
#include "aegis/graph.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version of the checkpoint format. Bump on breaking changes. */
#define AEGIS_CHECKPOINT_ABI_VERSION 1u

/** Maximum checkpoint filename length (including path). */
#define AEGIS_CHECKPOINT_PATH_MAX 512u

/** Header magic bytes identifying a valid checkpoint file. */
#define AEGIS_CHECKPOINT_MAGIC "AEGISCHK"
/** Header magic length. */
#define AEGIS_CHECKPOINT_MAGIC_LEN 8u

/* ── Checkpoint status ─────────────────────────────────────────────────────── */

/** Status of a checkpoint load operation. */
typedef enum aegis_checkpoint_status {
    AEGIS_CHECKPOINT_OK,               /**< Checkpoint valid and loaded.        */
    AEGIS_CHECKPOINT_MISSING,          /**< No checkpoint file at path.         */
    AEGIS_CHECKPOINT_CORRUPTED,        /**< Checksum mismatch or invalid magic. */
    AEGIS_CHECKPOINT_INCOMPLETE,       /**< File truncated / partial write.     */
    AEGIS_CHECKPOINT_VERSION_MISMATCH, /**< ABI version incompatibility.     */
} aegis_checkpoint_status_t;

/* ── Task snapshot ─────────────────────────────────────────────────────────── */

/**
 * @brief Snapshot of a single task's state for checkpointing.
 */
typedef struct aegis_checkpoint_task_snapshot {
    uint32_t task_id;        /**< Task ID (from aegis_task_id).      */
    char     task_name[64];  /**< Borrowed from task; copied here.   */
    int      task_state;     /**< aegis_task_state_t enum value.     */
    char     error_msg[256]; /**< Empty if no error.                 */
    int      retry_count;    /**< Number of retry attempts made.     */
    int      max_retries;    /**< From retry policy.                 */
} aegis_checkpoint_task_snapshot_t;

/* ── Checkpoint handle ─────────────────────────────────────────────────────── */

/** Opaque checkpoint handle. */
typedef struct aegis_checkpoint aegis_checkpoint_t;

/**
 * @brief Create an empty checkpoint.
 *
 * @param[out] out   Receives the checkpoint. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_checkpoint_create(aegis_checkpoint_t** out);

/**
 * @brief Destroy a checkpoint and release all owned resources.
 *
 * Safe to call with NULL.
 *
 * @param ckpt Checkpoint handle (ownership: consumed).
 */
void aegis_checkpoint_destroy(aegis_checkpoint_t* ckpt);

/* ── Population ────────────────────────────────────────────────────────────── */

/**
 * @brief Populate a checkpoint from an agent snapshot.
 *
 * Copies agent state, goal, plan (serialized), and enumerates the
 * task graph into per-task snapshots. Tasks in RUNNING state are
 * recorded as-is; the caller may post-process them before save.
 *
 * @param ckpt     Checkpoint (borrowed).
 * @param agent    Agent handle (borrowed; may be NULL — goal defaults empty).
 * @param plan     Current plan (borrowed; may be NULL — version defaults 0).
 * @param graph    Task graph (borrowed; may be NULL — no task snapshots).
 * @param version  Checkpoint version (monotonic; 0 = auto-increment from existing).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_checkpoint_populate(aegis_checkpoint_t* ckpt, const aegis_agent_t* agent,
                                         const aegis_plan_t* plan, const aegis_task_graph_t* graph,
                                         uint32_t version);

/**
 * @brief Get the checkpoint version.
 */
uint32_t aegis_checkpoint_version(const aegis_checkpoint_t* ckpt);

/**
 * @brief Get the checkpoint timestamp (seconds since epoch).
 */
uint64_t aegis_checkpoint_timestamp(const aegis_checkpoint_t* ckpt);

/**
 * @brief Get the agent goal text (borrowed).
 */
const char* aegis_checkpoint_goal(const aegis_checkpoint_t* ckpt);

/**
 * @brief Set the goal text on a checkpoint (copies string).
 *
 * Useful for orchestrators that have a goal string without an aegis_agent_t.
 *
 * @param ckpt Checkpoint (borrowed).
 * @param goal Goal text (borrowed; NULL clears).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_checkpoint_set_goal(aegis_checkpoint_t* ckpt, const char* goal);

/**
 * @brief Get the plan version captured in the checkpoint.
 */
uint32_t aegis_checkpoint_plan_version(const aegis_checkpoint_t* ckpt);

/**
 * @brief Get the plan DSL text (borrowed; may be NULL if plan was NULL).
 */
const char* aegis_checkpoint_plan_text(const aegis_checkpoint_t* ckpt);

/**
 * @brief Get the agent state string (borrowed).
 */
const char* aegis_checkpoint_agent_state(const aegis_checkpoint_t* ckpt);

/**
 * @brief Number of task snapshots in the checkpoint.
 */
size_t aegis_checkpoint_task_count(const aegis_checkpoint_t* ckpt);

/**
 * @brief Get a task snapshot by index.
 *
 * @param ckpt  Checkpoint (borrowed).
 * @param idx   Zero-based index.
 * @return Pointer to snapshot (borrowed; valid until checkpoint destroy).
 */
const aegis_checkpoint_task_snapshot_t* aegis_checkpoint_task_snapshot(
    const aegis_checkpoint_t* ckpt, size_t idx);

/* ── Serialization ─────────────────────────────────────────────────────────── */

/**
 * @brief Serialize the checkpoint to a JSON-like text format.
 *
 * Output format (human-readable, deterministic):
 *   CHECKPOINT v<N> ts=<timestamp> agentsate=<state> goal=<goal>
 *   PLAN v<N>
 *   <serialized plan text>
 *   TASK id=<id> name=<name> state=<state> error=<msg> retries=<n>/<max>
 *   ...
 *
 * The caller owns the returned string; free it with free().
 *
 * @param ckpt      Checkpoint (borrowed).
 * @param[out] out Receives the malloc'd string.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_checkpoint_serialize(const aegis_checkpoint_t* ckpt, char** out);

/* ── Deserialization ───────────────────────────────────────────────────────── */

/**
 * @brief Parse a checkpoint from text.
 *
 * Accepts output from aegis_checkpoint_serialize(). The parsed
 * checkpoint is ready for restore.
 *
 * @param text     Checkpoint text (borrowed; required).
 * @param[out] out Receives the parsed checkpoint. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (parse failure), or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_checkpoint_deserialize(const char* text, aegis_checkpoint_t** out);

/* ── Atomic write ──────────────────────────────────────────────────────────── */

/**
 * @brief Write a checkpoint to disk atomically.
 *
 * Writes to a temporary file in the same directory, then rename()s
 * it to the target path. This ensures readers never see a partial
 * file. If the target already exists, it is atomically replaced.
 *
 * @param ckpt       Checkpoint to write (borrowed).
 * @param path       Destination path (borrowed; required).
 * @param token      Cancellation token (borrowed; may be NULL).
 * @return AEGIS_OK, AEGIS_ERR_NOMEM, AEGIS_ERR_CANCELLED, or
 *         AEGIS_ERR_INTERNAL (I/O error).
 */
aegis_status_t aegis_checkpoint_write(const aegis_checkpoint_t* ckpt, const char* path,
                                      const aegis_cancellation_token_t* token);

/* ── Restore ───────────────────────────────────────────────────────────────── */

/**
 * @brief Read and verify a checkpoint from disk.
 *
 * Verifies:
 *   1. File exists and is readable.
 *   2. Magic bytes match.
 *   3. ABI version matches.
 *   4. Checksum is valid.
 *
 * On success, returns AEGIS_CHECKPOINT_OK with the parsed handle.
 * On failure, returns the appropriate status code.
 *
 * @param path       Checkpoint file path (borrowed; required).
 * @param[out] out   Receives the checkpoint. Ownership: transferred on OK.
 * @param[out] status Receives the load status (may be NULL).
 * @return AEGIS_OK on success (status indicates outcome), or a negative
 *         error code for I/O errors.
 */
aegis_status_t aegis_checkpoint_read(const char* path, aegis_checkpoint_t** out,
                                     aegis_checkpoint_status_t* status);

/**
 * @brief Get a human-readable string for a checkpoint status.
 */
const char* aegis_checkpoint_status_str(aegis_checkpoint_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CHECKPOINT_H */
