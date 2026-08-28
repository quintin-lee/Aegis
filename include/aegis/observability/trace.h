/**
 * @file trace.h
 * @brief Distributed tracing with trace_id, task_id, and agent_id propagation.
 *
 * Traces provide visibility into the execution path of an agent's work:
 *   - trace_id: unique identifier for a top-level operation
 *   - span_id: unique identifier for a single operation within a trace
 *   - parent_span_id: links to the parent span (NULL for root spans)
 *   - task_id: correlates spans to specific tasks
 *   - agent_id: correlates spans to a specific agent
 *
 * Key invariants:
 *   - Traces are created at the top level and propagated through async
 *     boundaries via aegis_trace_context_t.
 *   - Spans are created within traces and automatically closed on
 *     destruction.
 *   - Trace context is immutable once created (thread-safe to read).
 *   - The global trace scope provides the current active trace context
 *     for the calling thread.
 */
#ifndef AEGIS_TRACE_H
#define AEGIS_TRACE_H

#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Trace context ─────────────────────────────────────────────────────────── */

/**
 * @brief Opaque trace context carrying trace/span IDs across boundaries.
 *
 * Immutable once created. Safe to share across threads.
 */
typedef struct aegis_trace_context aegis_trace_context_t;

/* ── Span ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque span representing a single operation within a trace.
 */
typedef struct aegis_trace_span aegis_trace_span_t;

/* ── Trace lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Create a new trace context with a fresh trace ID.
 *
 * @param[out] out Receives the new context. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_trace_context_create(aegis_trace_context_t** out);

/**
 * @brief Clone a trace context, inheriting the same trace ID.
 *
 * Creates a new root span (parent_span_id = NULL) within the same trace.
 *
 * @param ctx Trace context to clone (borrowed).
 * @param[out] out Receives the cloned context. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_trace_context_clone(const aegis_trace_context_t* ctx,
                                         aegis_trace_context_t**      out);

/**
 * @brief Destroy a trace context and release all resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param ctx Handle to destroy (ownership: consumed).
 */
void aegis_trace_context_destroy(aegis_trace_context_t* ctx);

/* ── Span lifecycle ────────────────────────────────────────────────────────── */

/**
 * @brief Create a new span within a trace context.
 *
 * The new span's parent is the current span in @p ctx (or NULL if root).
 *
 * @param ctx  Trace context (mutable, borrowed).
 * @param name Span name (borrowed, must remain valid for span lifetime).
 * @param[out] out Receives the new span. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_trace_span_create(aegis_trace_context_t* ctx, const char* name,
                                       aegis_trace_span_t** out);

/**
 * @brief End a span and record its duration.
 *
 * Must be called exactly once per span. After this call, the span is
 * invalid and must not be used.
 *
 * @param span Handle to end (ownership: consumed).
 */
void aegis_trace_span_end(aegis_trace_span_t* span);

/**
 * @brief Destroy a span without ending it (aborted span).
 *
 * Safe to call with NULL (no-op).
 *
 * @param span Handle to destroy (ownership: consumed).
 */
void aegis_trace_span_destroy(aegis_trace_span_t* span);

/* ── Context accessors ─────────────────────────────────────────────────────── */

/**
 * @brief Get the trace ID from a context.
 *
 * @param ctx Trace context (borrowed).
 * @return Trace ID (0 if ctx is NULL).
 */
uint64_t aegis_trace_context_trace_id(const aegis_trace_context_t* ctx);

/**
 * @brief Get the current span ID from a context.
 *
 * @param ctx Trace context (borrowed).
 * @return Span ID (0 if ctx is NULL or has no active span).
 */
uint64_t aegis_trace_context_span_id(const aegis_trace_context_t* ctx);

/**
 * @brief Get the parent span ID from a context.
 *
 * @param ctx Trace context (borrowed).
 * @return Parent span ID (0 if root span).
 */
uint64_t aegis_trace_context_parent_span_id(const aegis_trace_context_t* ctx);

/**
 * @brief Get the agent ID associated with a context.
 *
 * @param ctx Trace context (borrowed).
 * @return Agent ID (0 if not set).
 */
uint64_t aegis_trace_context_agent_id(const aegis_trace_context_t* ctx);

/**
 * @brief Set the agent ID on a trace context.
 *
 * @param ctx  Trace context (mutable, borrowed).
 * @param agent_id Agent ID to associate.
 */
void aegis_trace_context_set_agent_id(aegis_trace_context_t* ctx, uint64_t agent_id);

/* ── Span accessors ────────────────────────────────────────────────────────── */

/**
 * @brief Get the span ID.
 *
 * @param span Span handle (borrowed).
 * @return Span ID.
 */
uint64_t aegis_trace_span_id(const aegis_trace_span_t* span);

/**
 * @brief Get the trace ID this span belongs to.
 *
 * @param span Span handle (borrowed).
 * @return Trace ID.
 */
uint64_t aegis_trace_span_trace_id(const aegis_trace_span_t* span);

/**
 * @brief Get the parent span ID.
 *
 * @param span Span handle (borrowed).
 * @return Parent span ID (0 if root).
 */
uint64_t aegis_trace_span_parent_id(const aegis_trace_span_t* span);

/**
 * @brief Get the span name.
 *
 * @param span Span handle (borrowed).
 * @return Span name (borrowed).
 */
const char* aegis_trace_span_name(const aegis_trace_span_t* span);

/**
 * @brief Get the span duration in microseconds.
 *
 * Returns 0 if the span has not been ended.
 *
 * @param span Span handle (borrowed).
 * @return Duration in microseconds.
 */
uint64_t aegis_trace_span_duration_us(const aegis_trace_span_t* span);

/* ── Global trace scope ────────────────────────────────────────────────────── */

/**
 * @brief Set the thread-local active trace context.
 *
 * This is the mechanism by which trace context is propagated across
 * async boundaries. Each thread maintains its own active context.
 *
 * @param ctx New active context (borrowed; does not take ownership).
 */
void aegis_trace_set_active(const aegis_trace_context_t* ctx);

/**
 * @brief Get the thread-local active trace context.
 *
 * @return Active context (borrowed), or NULL if none set.
 */
const aegis_trace_context_t* aegis_trace_get_active(void);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TRACE_H */
