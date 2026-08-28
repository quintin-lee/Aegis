/**
 * @file reflection.h
 * @brief Outcome summary of an executed task graph, as feedback for replanning.
 *
 * Reflection closes the planning loop: after a materialized plan's task
 * graph has run (or failed), it condenses what happened into counts per
 * terminal state plus the first failure description, and renders a
 * ready-to-use feedback string for aegis_replan().
 *
 * It reads ONLY public task/graph accessors; it never mutates the graph
 * and never executes anything.
 *
 * Ownership: the reflection object copies everything it reports (the
 * graph may be destroyed immediately after create()).
 *
 * Thread safety: instances are plain value containers built once;
 * concurrent reads of a finished instance are safe.
 */
#ifndef AEGIS_REFLECTION_H
#define AEGIS_REFLECTION_H

#include "aegis/task/graph.h"
#include "aegis/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque reflection handle. */
typedef struct aegis_reflection aegis_reflection_t;

/**
 * @brief Summarize a task graph's execution outcome.
 *
 * @param[out] out   Receives the reflection. Ownership: transferred.
 * @param[in]  graph Task graph to summarize (borrowed; read-only).
 * @return AEGIS_OK, AEGIS_ERR_INVALID or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_reflection_create(aegis_reflection_t** out, const aegis_task_graph_t* graph);

/** Destroy the reflection. Safe to call with NULL. */
void aegis_reflection_destroy(aegis_reflection_t* refl);

/** Tasks that reached SUCCESS. */
size_t aegis_reflection_success_count(const aegis_reflection_t* refl);

/** Tasks that ended FAILED (retries exhausted). */
size_t aegis_reflection_failed_count(const aegis_reflection_t* refl);

/** Tasks that were cancelled by the caller. */
size_t aegis_reflection_cancelled_count(const aegis_reflection_t* refl);

/** Tasks skipped because a predecessor failed or was cancelled. */
size_t aegis_reflection_skipped_count(const aegis_reflection_t* refl);

/** Tasks not in any terminal state (pending/ready/running/waiting). */
size_t aegis_reflection_incomplete_count(const aegis_reflection_t* refl);

/**
 * @brief Error message of the first FAILED task encountered (in graph
 *        enumeration order).
 *
 * @return Borrowed string owned by the reflection, or NULL when no task
 *         failed with an error message.
 */
const char* aegis_reflection_first_error(const aegis_reflection_t* refl);

/**
 * @brief Rendered feedback paragraph for aegis_replan().
 *
 * @return Borrowed NUL-terminated string owned by the reflection
 *         (never NULL for a valid instance).
 */
const char* aegis_reflection_feedback(const aegis_reflection_t* refl);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_REFLECTION_H */
