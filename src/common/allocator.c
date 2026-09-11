/**
 * @file allocator.c
 * @brief Pluggable allocator implementation with optional stats tracking.
 *
 * Provides:
 * - a singleton default (system malloc/free/realloc) allocator
 * - a tracking wrapper that counts allocations and reports byte-level stats
 *
 * The tracking allocator's context is heap-allocated and must be freed
 * via aegis_alloc_tracker_destroy() to avoid leaks.
 */
#include "aegis/common/allocator.h"
#include <stdlib.h>
#include <string.h>

/* ── Default (system) allocator singleton ─────────────────────────────────── */

/**
 * @brief System malloc backend for the default allocator.
 *
 * @param self Unused (stateless backend).
 * @param size Bytes to allocate; 0 yields NULL.
 * @param ctx  Unused.
 * @return Fresh heap block, or NULL on failure or zero size.
 */
static void* sys_alloc(aegis_allocator_t* self, size_t size, void* ctx)
{
    (void)self;
    (void)ctx;
    return size ? malloc(size) : NULL;
}
/**
 * @brief System free backend for the default allocator.
 *
 * Safe to call with NULL (no-op via free).
 *
 * @param self Unused.
 * @param ptr  Block to release (borrowed; may be NULL).
 * @param ctx  Unused.
 */
static void sys_free(aegis_allocator_t* self, void* ptr, void* ctx)
{
    (void)self;
    (void)ctx;
    free(ptr);
}
/**
 * @brief System realloc backend for the default allocator.
 *
 * @param self     Unused.
 * @param ptr      Block to resize (may be NULL, behaves as malloc).
 * @param old_size Unused; kept for interface symmetry.
 * @param new_size New size in bytes.
 * @param ctx      Unused.
 * @return Resized block, or NULL on failure.
 */
static void* sys_realloc(aegis_allocator_t* self, void* ptr, size_t old_size, size_t new_size,
                          void* ctx)
{
    (void)self;
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}
/**
 * @brief Stats backend for the default allocator (no tracking).
 *
 * The system allocator keeps no counters, so this is a no-op that
 * leaves @p out untouched.
 *
 * @param self Unused.
 * @param out  Unused.
 * @param ctx  Unused.
 */
static void sys_stats(const aegis_allocator_t* self, aegis_alloc_stats_t* out, void* ctx)
{
    (void)self;
    (void)out;
    (void)ctx;
}
/**
 * @brief Destroy backend for the default allocator (no-op).
 *
 * The singleton is statically allocated and must never be freed.
 *
 * @param self Unused.
 * @param ctx  Unused.
 */
static void sys_destroy(aegis_allocator_t* self, void* ctx)
{
    (void)self;
    (void)ctx;
}

/** Singleton system allocator instance (statically allocated, never destroyed). */
static const aegis_allocator_t k_sys_default = {
    .alloc   = sys_alloc,
    .free    = sys_free,
    .realloc = sys_realloc,
    .stats   = sys_stats,
    .destroy = sys_destroy,
    .ctx     = NULL,
};

/**
 * @brief Return the process-wide default (system) allocator.
 *
 * @return Borrowed pointer to the static singleton; never NULL, do not free.
 */
const aegis_allocator_t* aegis_alloc_default(void)
{
    return &k_sys_default;
}

/**
 * @brief Allocate @p size bytes via @p alloc, or malloc when NULL.
 *
 * @param alloc Allocator to use (borrowed; NULL selects system malloc).
 * @param size  Bytes to allocate.
 * @return Fresh block (ownership: transferred), or NULL on failure.
 */
void* aegis_alloc(const aegis_allocator_t* alloc, size_t size)
{
    if (!alloc) {
        return malloc(size);
    }
    return alloc->alloc((aegis_allocator_t*)alloc, size, alloc->ctx);
}

/**
 * @brief Release a block via @p alloc, or free when NULL/empty.
 *
 * A NULL allocator or NULL pointer falls back to system free (no-op on NULL).
 *
 * @param alloc Allocator that owns the block (borrowed; may be NULL).
 * @param ptr   Block to release (ownership: consumed; may be NULL).
 */
void aegis_free(const aegis_allocator_t* alloc, void* ptr)
{
    if (!alloc || !ptr) {
        free((void*)ptr);
        return;
    }
    alloc->free((aegis_allocator_t*)alloc, ptr, alloc->ctx);
}

/**
 * @brief Resize a block via @p alloc, or realloc when NULL.
 *
 * @param alloc    Allocator that owns the block (borrowed; may be NULL).
 * @param ptr      Block to resize (may be NULL, behaves as alloc).
 * @param old_size Previous size in bytes (backend-specific use).
 * @param new_size New size in bytes.
 * @return Resized block (ownership: transferred), or NULL on failure.
 */
void* aegis_realloc(const aegis_allocator_t* alloc, void* ptr, size_t old_size, size_t new_size)
{
    if (!alloc) {
        return realloc(ptr, new_size);
    }
    return alloc->realloc((aegis_allocator_t*)alloc, ptr, old_size, new_size, alloc->ctx);
}

/**
 * @brief Query allocation statistics from @p alloc.
 *
 * No-op when @p alloc or @p stats is NULL.
 *
 * @param alloc Allocator to query (borrowed).
 * @param[out] stats Receives a snapshot copy of the counters.
 */
void aegis_alloc_stats(const aegis_allocator_t* alloc, aegis_alloc_stats_t* stats)
{
    if (!alloc || !stats) {
        return;
    }
    alloc->stats(alloc, stats, alloc->ctx);
}

/* ── Tracking allocator ───────────────────────────────────────────────────── */

/** Heap-allocated context of a tracking allocator: wrapped base + live counters. */
typedef struct {
    aegis_allocator_t   base; /**< Wrapped base allocator (copied at creation). */
    aegis_alloc_stats_t st;   /**< Cumulative allocation statistics. */
} tracking_ctx_t;

/**
 * @brief Allocate via the wrapped base and record statistics.
 *
 * Falls back to the system allocator when the wrapped base has no alloc entry.
 * Only successful allocations update the counters and the peak watermark.
 *
 * @param self Unused (context travels via @p ctx).
 * @param size Bytes to allocate.
 * @param ctx  Tracking context (borrowed).
 * @return Fresh block (ownership: transferred), or NULL on failure.
 */
static void* track_alloc(aegis_allocator_t* self, size_t size, void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    void*                    p    = base->alloc((aegis_allocator_t*)base, size, base->ctx);
    if (p) {
        tc->st.allocations++;
        tc->st.bytes_allocated += size;
        tc->st.current_bytes += size;
        if (tc->st.current_bytes > tc->st.peak_bytes) {
            tc->st.peak_bytes = tc->st.current_bytes;
        }
    }
    return p;
}
/**
 * @brief Release a block via the wrapped base and count the deallocation.
 *
 * Note: freed byte counts are not tracked (size unknown at free time);
 * only the deallocation counter is incremented.
 *
 * @param self Unused.
 * @param ptr  Block to release (ownership: consumed).
 * @param ctx  Tracking context (borrowed).
 */
static void track_free(aegis_allocator_t* self, void* ptr, void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    tc->st.deallocations++;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    base->free((aegis_allocator_t*)base, ptr, base->ctx);
}
/**
 * @brief Resize via the wrapped base and account the size delta.
 *
 * Deltas are recorded only when realloc returns a different pointer;
 * in-place resizes keep the counters unchanged (conservative estimate).
 *
 * @param self     Unused.
 * @param ptr      Block to resize (may be NULL).
 * @param old_size Previous size in bytes.
 * @param new_size New size in bytes.
 * @param ctx      Tracking context (borrowed).
 * @return Resized block (ownership: transferred), or NULL on failure.
 */
static void* track_realloc(aegis_allocator_t* self, void* ptr, size_t old_size, size_t new_size,
                            void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    void* p = base->realloc((aegis_allocator_t*)base, ptr, old_size, new_size, base->ctx);
    if (p && p != ptr) {
        tc->st.bytes_allocated += (new_size > old_size ? new_size - old_size : 0);
        tc->st.bytes_freed += (old_size > new_size ? old_size - new_size : 0);
    }
    return p;
}
/**
 * @brief Copy the live tracking counters into @p out.
 *
 * @param self Unused.
 * @param[out] out Receives a snapshot copy (no-op when NULL).
 * @param ctx  Tracking context (borrowed).
 */
static void track_stats(const aegis_allocator_t* self, aegis_alloc_stats_t* out, void* ctx)
{
    (void)self;
    if (out) {
        *out = ((tracking_ctx_t*)ctx)->st;
    }
}
/**
 * @brief Reset a tracking allocator in place (zero stats, detach base).
 *
 * Does NOT free the tracking context itself; the caller owns @p ctx and
 * must release it (see aegis_alloc_tracker_destroy).
 *
 * @param self Unused.
 * @param ctx  Tracking context to reset (borrowed).
 */
static void track_destroy(aegis_allocator_t* self, void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    /* Do NOT free tc — the tracker is stack-allocated by caller.
     * Only clear the stats and let caller free tc if they want. */
    memset(&tc->st, 0, sizeof(tc->st));
    tc->base.alloc = NULL;
}

/**
 * @brief Create a tracking allocator wrapping @p base (by value).
 *
 * The returned handle is returned by value; its heap-allocated context
 * must be released with aegis_alloc_tracker_destroy to avoid leaks.
 * A NULL @p base selects the system allocator. On allocation failure a
 * zeroed (unusable) handle is returned.
 *
 * @param base Allocator to wrap (borrowed; may be NULL).
 * @return Tracking allocator handle (owns its context).
 */
aegis_allocator_t aegis_alloc_tracker(const aegis_allocator_t* base)
{
    const aegis_allocator_t* b  = base ? base : aegis_alloc_default();
    tracking_ctx_t*          tc = calloc(1, sizeof(*tc));
    if (!tc) {
        aegis_allocator_t bad = {0};
        return bad;
    }
    tc->base                  = *b;
    aegis_allocator_t tracker = {
        .alloc   = track_alloc,
        .free    = track_free,
        .realloc = track_realloc,
        .stats   = track_stats,
        .destroy = track_destroy,
        .ctx     = tc,
    };
    return tracker;
}

/**
 * @brief Destroy a tracking allocator and free its context.
 *
 * Destroys the wrapped base's own allocations first, then frees the
 * tracking context and returns a zeroed handle. Handles without a
 * destroy entry or context are returned unchanged.
 *
 * @param tracker Tracker to tear down (ownership: consumed).
 * @return Zeroed allocator handle (no longer usable).
 */
aegis_allocator_t aegis_alloc_tracker_destroy(aegis_allocator_t tracker)
{
    if (!tracker.destroy || !tracker.ctx) {
        return tracker;
    }
    tracking_ctx_t*          tc   = (tracking_ctx_t*)tracker.ctx;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    /* Free the base allocator's own allocations (if any) */
    base->destroy((aegis_allocator_t*)base, base->ctx);
    /* Free the tracking context */
    aegis_free(base, tc);
    memset(&tracker, 0, sizeof(tracker));
    return tracker;
}
