/**
 * @file trace.c
 * @brief Distributed tracing implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/observability/trace.h"
#include "aegis/status.h"
#include <stdatomic.h>

#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* ── Internal types ────────────────────────────────────────────────────────── */

#define AEGIS_TRACE_MAX_SPANS 64

typedef struct aegis_trace_span {
    uint64_t    id;
    uint64_t    trace_id;
    uint64_t    parent_id;
    const char* name;
    uint64_t    start_ns;
    uint64_t    end_ns; /**< 0 if not yet ended.                */
    int         ended;  /**< Non-zero if span has been ended.    */
} aegis_trace_span_t;

struct aegis_trace_context {
    uint64_t           trace_id;
    uint64_t           agent_id;
    aegis_trace_span_t spans[AEGIS_TRACE_MAX_SPANS];
    size_t             n_spans;
    size_t             current_span; /**< Index of the active (most recently created) span. */
};

/* ── ID generator ──────────────────────────────────────────────────────────── */

static uint64_t g_next_id = 1;

static uint64_t trace_generate_id(void)
{
    return g_next_id++;
}

static uint64_t trace_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ── Thread-local active context ───────────────────────────────────────────── */

#if defined(__clang__) || defined(__GNUC__)
#define TLS __thread
#else
#define TLS
#endif

static TLS const aegis_trace_context_t* g_active_ctx = NULL;

/* ── Trace context lifecycle ───────────────────────────────────────────────── */

aegis_status_t aegis_trace_context_create(aegis_trace_context_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_trace_context_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }
    ctx->trace_id = trace_generate_id();
    ctx->agent_id = 0;
    *out          = ctx;
    return AEGIS_OK;
}

aegis_status_t aegis_trace_context_clone(const aegis_trace_context_t* ctx,
                                         aegis_trace_context_t**      out)
{
    AEGIS_CHECK_OUT(out);
    if (!ctx) {
        return AEGIS_ERR_INVALID;
    }
    aegis_trace_context_t* clone = calloc(1, sizeof(*clone));
    if (!clone) {
        return AEGIS_ERR_NOMEM;
    }
    clone->trace_id = ctx->trace_id;
    clone->agent_id = ctx->agent_id;
    /* Clone spans up to current. */
    size_t n = ctx->n_spans < AEGIS_TRACE_MAX_SPANS ? ctx->n_spans : AEGIS_TRACE_MAX_SPANS;
    memcpy(clone->spans, ctx->spans, sizeof(aegis_trace_span_t) * n);
    clone->n_spans      = n;
    clone->current_span = ctx->current_span < n ? ctx->current_span : (n > 0 ? n - 1 : 0);
    *out                = clone;
    return AEGIS_OK;
}

/**
 * @brief Destroy a trace context and free its heap allocation.
 *
 * Spans are stored inline in the context; there is nothing per-span to free.
 * NULL is a no-op.
 *
 * @param[in] ctx Trace context to destroy, or NULL.
 */
void aegis_trace_context_destroy(aegis_trace_context_t* ctx)
{
    free(ctx);
}

/* ── Span lifecycle ────────────────────────────────────────────────────────── */

/**
 * @brief Create a new trace span within the given context.
 *
 * Spans are stored in a fixed-size array (AEGIS_TRACE_MAX_SPANS = 64);
 * when full the call returns AEGIS_ERR_BUSY. The new span's parent is
 * the most recently created span. The name pointer is borrowed (not
 * copied) — it must outlive the span. The caller does NOT own the span;
 * it lives inside the context and is destroyed when the context is
 * destroyed.
 *
 * @param[in]  ctx  Trace context to extend (must be non-NULL).
 * @param[in]  name Span name (borrowed pointer, must remain valid).
 * @param[out] out  Receives a pointer into the context's span array.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY when the span table is full.
 */
aegis_status_t aegis_trace_span_create(aegis_trace_context_t* ctx, const char* name,
                                       aegis_trace_span_t** out)
{
    AEGIS_CHECK_OUT(out);
    if (!ctx || !name) {
        return AEGIS_ERR_INVALID;
    }
    if (ctx->n_spans >= AEGIS_TRACE_MAX_SPANS) {
        return AEGIS_ERR_BUSY;
    }

    size_t              idx  = ctx->n_spans++;
    aegis_trace_span_t* span = &ctx->spans[idx];
    span->id                 = trace_generate_id();
    span->trace_id           = ctx->trace_id;
    span->parent_id          = idx > 0 ? ctx->spans[idx - 1].id : 0;
    span->name               = name;
    span->start_ns           = trace_now_ns();
    span->end_ns             = 0;
    span->ended              = 0;

    ctx->current_span = idx;
    *out              = span;
    return AEGIS_OK;
}

/**
 * @brief Mark a span as ended, recording the end timestamp.
 *
 * Idempotent: calling end() multiple times on the same span is a no-op
 * after the first call. NULL is a no-op.
 *
 * @param[in] span Span to end, or NULL.
 */
void aegis_trace_span_end(aegis_trace_span_t* span)
{
    if (!span || span->ended) {
        return;
    }
    span->end_ns = trace_now_ns();
    span->ended  = 1;
}

void aegis_trace_span_destroy(aegis_trace_span_t* span)
{
    /* Spans are owned by the context; this is a no-op for safety. */
    (void)span;
}

/* ── Context accessors ─────────────────────────────────────────────────────── */

/**
 * @brief Return the trace id for the given context.
 *
 * All spans in the same context share the same trace_id.
 *
 * @param[in] ctx Trace context, or NULL.
 * @return Trace id, or 0 for NULL.
 */
uint64_t aegis_trace_context_trace_id(const aegis_trace_context_t* ctx)
{
    return ctx ? ctx->trace_id : 0;
}

/**
 * @brief Return the active (most recently created) span id.
 *
 * Returns 0 when the context has no spans.
 *
 * @param[in] ctx Trace context, or NULL.
 * @return Active span id, or 0.
 */
uint64_t aegis_trace_context_span_id(const aegis_trace_context_t* ctx)
{
    if (!ctx || ctx->n_spans == 0) {
        return 0;
    }
    return ctx->spans[ctx->current_span].id;
}

/**
 * @brief Return the parent span id of the active span.
 *
 * Returns 0 when there is no active span or no parent (root span).
 *
 * @param[in] ctx Trace context, or NULL.
 * @return Parent span id, or 0.
 */
uint64_t aegis_trace_context_parent_span_id(const aegis_trace_context_t* ctx)
{
    if (!ctx || ctx->n_spans == 0) {
        return 0;
    }
    return ctx->spans[ctx->current_span].parent_id;
}

/**
 * @brief Return the agent id associated with the trace context.
 *
 * Set via aegis_trace_context_set_agent_id(). Defaults to 0.
 *
 * @param[in] ctx Trace context, or NULL.
 * @return Agent id, or 0.
 */
uint64_t aegis_trace_context_agent_id(const aegis_trace_context_t* ctx)
{
    return ctx ? ctx->agent_id : 0;
}

/**
 * @brief Set the agent id on a trace context.
 *
 * NULL is a no-op.
 *
 * @param[in] ctx      Trace context to update.
 * @param[in] agent_id New agent id.
 */
void aegis_trace_context_set_agent_id(aegis_trace_context_t* ctx, uint64_t agent_id)
{
    if (ctx) {
        ctx->agent_id = agent_id;
    }
}

/* ── Span accessors ────────────────────────────────────────────────────────── */

/**
 * @brief Return the span's unique id.
 *
 * @param[in] span Span, or NULL.
 * @return Span id, or 0 for NULL.
 */
uint64_t aegis_trace_span_id(const aegis_trace_span_t* span)
{
    return span ? span->id : 0;
}

/**
 * @brief Return the span's trace id (shared across all spans in one trace).
 *
 * @param[in] span Span, or NULL.
 * @return Trace id, or 0 for NULL.
 */
uint64_t aegis_trace_span_trace_id(const aegis_trace_span_t* span)
{
    return span ? span->trace_id : 0;
}

/**
 * @brief Return the span's parent span id.
 *
 * @param[in] span Span, or NULL.
 * @return Parent span id, or 0 for NULL / root span.
 */
uint64_t aegis_trace_span_parent_id(const aegis_trace_span_t* span)
{
    return span ? span->parent_id : 0;
}

/**
 * @brief Borrow the span name string (borrowed, not owned).
 *
 * @param[in] span Span, or NULL.
 * @return Name text, or NULL for NULL span.
 */
const char* aegis_trace_span_name(const aegis_trace_span_t* span)
{
    return span ? span->name : NULL;
}

/**
 * @brief Return the span duration in microseconds.
 *
 * Returns 0 when the span has not yet been ended or @p span is NULL.
 *
 * @param[in] span Span, or NULL.
 * @return Duration in microseconds, or 0.
 */
uint64_t aegis_trace_span_duration_us(const aegis_trace_span_t* span)
{
    if (!span || !span->ended) {
        return 0;
    }
    return (span->end_ns - span->start_ns) / 1000;
}

/* ── Global trace scope ────────────────────────────────────────────────────── */

/**
 * @brief Set the thread-local active trace context.
 *
 * Subsequent span creations will use this context as the parent chain.
 * This is a global (per-thread) setter with no lock — callers must
 * coordinate access themselves.
 *
 * @param[in] ctx Active context, or NULL to clear.
 */
void aegis_trace_set_active(const aegis_trace_context_t* ctx)
{
    g_active_ctx = ctx;
}

/**
 * @brief Return the current thread-local active trace context.
 *
 * @return Active context pointer, or NULL when none is set.
 */
const aegis_trace_context_t* aegis_trace_get_active(void)
{
    return g_active_ctx;
}
