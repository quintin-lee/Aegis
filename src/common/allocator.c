#include "aegis/common/allocator.h"
#include <stdlib.h>
#include <string.h>

/* ── Default (system) allocator singleton ─────────────────────────────────── */

static void* sys_alloc(aegis_allocator_t* self, size_t size, void* ctx)
{
    (void)self;
    (void)ctx;
    return size ? malloc(size) : NULL;
}
static void sys_free(aegis_allocator_t* self, void* ptr, void* ctx)
{
    (void)self;
    (void)ctx;
    free(ptr);
}
static void* sys_realloc(aegis_allocator_t* self, void* ptr, size_t old_size, size_t new_size,
                         void* ctx)
{
    (void)self;
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}
static void sys_stats(const aegis_allocator_t* self, aegis_alloc_stats_t* out, void* ctx)
{
    (void)self;
    (void)out;
    (void)ctx;
}
static void sys_destroy(aegis_allocator_t* self, void* ctx)
{
    (void)self;
    (void)ctx;
}

static const aegis_allocator_t k_sys_default = {
    .alloc   = sys_alloc,
    .free    = sys_free,
    .realloc = sys_realloc,
    .stats   = sys_stats,
    .destroy = sys_destroy,
    .ctx     = NULL,
};

const aegis_allocator_t* aegis_alloc_default(void)
{
    return &k_sys_default;
}

void* aegis_alloc(const aegis_allocator_t* alloc, size_t size)
{
    if (!alloc)
        return malloc(size);
    return alloc->alloc((aegis_allocator_t*)alloc, size, alloc->ctx);
}

void aegis_free(const aegis_allocator_t* alloc, void* ptr)
{
    if (!alloc || !ptr) {
        free((void*)ptr);
        return;
    }
    alloc->free((aegis_allocator_t*)alloc, ptr, alloc->ctx);
}

void* aegis_realloc(const aegis_allocator_t* alloc, void* ptr, size_t old_size, size_t new_size)
{
    if (!alloc)
        return realloc(ptr, new_size);
    return alloc->realloc((aegis_allocator_t*)alloc, ptr, old_size, new_size, alloc->ctx);
}

void aegis_alloc_stats(const aegis_allocator_t* alloc, aegis_alloc_stats_t* stats)
{
    if (!alloc || !stats)
        return;
    alloc->stats(alloc, stats, alloc->ctx);
}

/* ── Tracking allocator ───────────────────────────────────────────────────── */

typedef struct {
    aegis_allocator_t   base;
    aegis_alloc_stats_t st;
} tracking_ctx_t;

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
        if (tc->st.current_bytes > tc->st.peak_bytes)
            tc->st.peak_bytes = tc->st.current_bytes;
    }
    return p;
}
static void track_free(aegis_allocator_t* self, void* ptr, void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    tc->st.deallocations++;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    base->free((aegis_allocator_t*)base, ptr, base->ctx);
}
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
static void track_stats(const aegis_allocator_t* self, aegis_alloc_stats_t* out, void* ctx)
{
    (void)self;
    if (out)
        *out = ((tracking_ctx_t*)ctx)->st;
}
static void track_destroy(aegis_allocator_t* self, void* ctx)
{
    tracking_ctx_t* tc = (tracking_ctx_t*)ctx;
    (void)self;
    /* Do NOT free tc — the tracker is stack-allocated by caller.
     * Only clear the stats and let caller free tc if they want. */
    memset(&tc->st, 0, sizeof(tc->st));
    tc->base.alloc = NULL;
}

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

aegis_allocator_t aegis_alloc_tracker_destroy(aegis_allocator_t tracker)
{
    if (!tracker.destroy || !tracker.ctx)
        return tracker;
    tracking_ctx_t*          tc   = (tracking_ctx_t*)tracker.ctx;
    const aegis_allocator_t* base = tc->base.alloc ? &tc->base : aegis_alloc_default();
    /* Free the base allocator's own allocations (if any) */
    base->destroy((aegis_allocator_t*)base, base->ctx);
    /* Free the tracking context */
    aegis_free(base, tc);
    memset(&tracker, 0, sizeof(tracker));
    return tracker;
}
