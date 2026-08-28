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

void aegis_trace_context_destroy(aegis_trace_context_t* ctx)
{
    free(ctx);
}

/* ── Span lifecycle ────────────────────────────────────────────────────────── */

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

uint64_t aegis_trace_context_trace_id(const aegis_trace_context_t* ctx)
{
    return ctx ? ctx->trace_id : 0;
}

uint64_t aegis_trace_context_span_id(const aegis_trace_context_t* ctx)
{
    if (!ctx || ctx->n_spans == 0) {
        return 0;
    }
    return ctx->spans[ctx->current_span].id;
}

uint64_t aegis_trace_context_parent_span_id(const aegis_trace_context_t* ctx)
{
    if (!ctx || ctx->n_spans == 0) {
        return 0;
    }
    return ctx->spans[ctx->current_span].parent_id;
}

uint64_t aegis_trace_context_agent_id(const aegis_trace_context_t* ctx)
{
    return ctx ? ctx->agent_id : 0;
}

void aegis_trace_context_set_agent_id(aegis_trace_context_t* ctx, uint64_t agent_id)
{
    if (ctx) {
        ctx->agent_id = agent_id;
    }
}

/* ── Span accessors ────────────────────────────────────────────────────────── */

uint64_t aegis_trace_span_id(const aegis_trace_span_t* span)
{
    return span ? span->id : 0;
}

uint64_t aegis_trace_span_trace_id(const aegis_trace_span_t* span)
{
    return span ? span->trace_id : 0;
}

uint64_t aegis_trace_span_parent_id(const aegis_trace_span_t* span)
{
    return span ? span->parent_id : 0;
}

const char* aegis_trace_span_name(const aegis_trace_span_t* span)
{
    return span ? span->name : NULL;
}

uint64_t aegis_trace_span_duration_us(const aegis_trace_span_t* span)
{
    if (!span || !span->ended) {
        return 0;
    }
    return (span->end_ns - span->start_ns) / 1000;
}

/* ── Global trace scope ────────────────────────────────────────────────────── */

void aegis_trace_set_active(const aegis_trace_context_t* ctx)
{
    g_active_ctx = ctx;
}

const aegis_trace_context_t* aegis_trace_get_active(void)
{
    return g_active_ctx;
}
