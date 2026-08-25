#ifndef AEGIS_ALLOCATOR_H
#define AEGIS_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file allocator.h
 * @brief Pluggable allocator interface with stats tracking.
 *
 * All allocators are stateful objects with explicit lifecycle.
 * Pass an allocator to any foundation function that needs heap memory.
 * Passing NULL uses the default (system) allocator.
 */

typedef struct aegis_allocator aegis_allocator_t;

/** Allocation stats — updated atomically by thread-safe allocators. */
typedef struct aegis_alloc_stats {
    uint64_t allocations;     /* total alloc calls      */
    uint64_t deallocations;   /* total free calls       */
    uint64_t bytes_allocated; /* cumulative bytes     */
    uint64_t bytes_freed;     /* cumulative bytes freed */
    uint64_t peak_bytes;      /* highest live bytes   */
    uint64_t current_bytes;   /* live bytes now       */
} aegis_alloc_stats_t;

/* ── Function pointers (vtable) ────────────────────────────────────────────── */

typedef void* (*alloc_fn)(aegis_allocator_t* self, size_t size, void* ctx);
typedef void (*free_fn)(aegis_allocator_t* self, void* ptr, void* ctx);
typedef void* (*realloc_fn)(aegis_allocator_t* self, void* ptr, size_t old_size, size_t new_size,
                            void* ctx);
typedef void (*stats_fn)(const aegis_allocator_t* self, aegis_alloc_stats_t* out, void* ctx);
typedef void (*destroy_fn)(aegis_allocator_t* self, void* ctx);

struct aegis_allocator {
    alloc_fn   alloc;
    free_fn    free;
    realloc_fn realloc;
    stats_fn   stats;
    destroy_fn destroy;
    void*      ctx; /* passed through to every callback */
};

/* ── Default (system) allocator ───────────────────────────────────────────── */

/**
 * @brief Return a pointer to the singleton default allocator.
 *
 * Read-only; do not modify. Lifetime is process-wide.
 */
const aegis_allocator_t* aegis_alloc_default(void);

/**
 * @brief Allocate size bytes.
 *
 * If alloc is NULL, falls back to system malloc.
 */
void* aegis_alloc(const aegis_allocator_t* alloc, size_t size);

/**
 * @brief Free a previously allocated block.
 */
void aegis_free(const aegis_allocator_t* alloc, void* ptr);

/**
 * @brief Reallocate a block to new_size bytes.
 *
 * Preserves content up to min(old_size, new_size).
 */
void* aegis_realloc(const aegis_allocator_t* alloc, void* ptr, size_t old_size, size_t new_size);

/**
 * @brief Query allocation statistics.
 *
 * If stats is NULL the call is a no-op.
 */
void aegis_alloc_stats(const aegis_allocator_t* alloc, aegis_alloc_stats_t* stats);

/* ── Tracking allocator (wraps another allocator) ──────────────────────────── */

/**
 * @brief Create a tracking allocator that wraps base and reports stats.
 *
 * The tracking allocator takes ownership of base (base is destroyed when
 * the tracker is destroyed). Passing NULL uses the system default as base.
 */
aegis_allocator_t aegis_alloc_tracker(const aegis_allocator_t* base);
/**
 * @brief Destroy a tracking allocator and free its internal context.
 */
aegis_allocator_t aegis_alloc_tracker_destroy(aegis_allocator_t tracker);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ALLOCATOR_H */
