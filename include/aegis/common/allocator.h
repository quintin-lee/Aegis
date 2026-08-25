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
 * @brief Pluggable allocator interface with optional stats tracking.
 *
 * All allocators are stateful objects with an explicit lifecycle
 * (create via factory or use the singleton default). Pass an allocator
 * pointer to any foundation function that needs heap memory.
 * Passing NULL falls back to the system allocator.
 *
 * The vtable (alloc / free / realloc / stats / destroy) allows
 * plugging in custom allocators — e.g. a slab allocator, a tracker,
 * or a sanitizer wrapper — without changing call sites.
 */

/** Opaque allocator handle. */
typedef struct aegis_allocator aegis_allocator_t;

/**
 * @brief Allocation statistics snapshot.
 *
 * Fields are updated by thread-safe allocators; read via
 * aegis_alloc_stats(). Values are approximate when read concurrently.
 */
typedef struct aegis_alloc_stats {
    uint64_t allocations;     /**< Total number of alloc calls.          */
    uint64_t deallocations;   /**< Total number of free calls.           */
    uint64_t bytes_allocated; /**< Cumulative bytes requested.           */
    uint64_t bytes_freed;     /**< Cumulative bytes returned to OS.      */
    uint64_t peak_bytes;      /**< Highest live-byte count observed.     */
    uint64_t current_bytes;   /**< Live bytes at last stats snapshot.    */
} aegis_alloc_stats_t;

/* ── Vtable function-pointer types ─────────────────────────────────────────── */

/** Allocate size bytes. Returns NULL on failure. */
typedef void* (*aegis_alloc_fn)(aegis_allocator_t* self, size_t size, void* ctx);
/** Free a previously allocated block. Safe with NULL ptr. */
typedef void  (*aegis_free_fn)(aegis_allocator_t* self, void* ptr, void* ctx);
/** Reallocate block; new_size may be 0 (equivalent to free). */
typedef void* (*aegis_realloc_fn)(aegis_allocator_t* self, void* ptr,
                                  size_t old_size, size_t new_size, void* ctx);
/** Fill out @p stats; if NULL the call is a no-op. */
typedef void  (*aegis_stats_fn)(const aegis_allocator_t* self,
                                aegis_alloc_stats_t* stats, void* ctx);
/** Destroy the allocator and release any internal resources. */
typedef void  (*aegis_destroy_fn)(aegis_allocator_t* self, void* ctx);

/**
 * @brief Allocator vtable.
 *
 * Every field must be non-NULL except stats and destroy, which may
 * be NULL if the allocator does not support those operations.
 *
 * @var aegis_allocator_t::alloc
 *   Allocate function. Must not return NULL when size > 0 unless
 *   the allocator signals OOM by some other means.
 * @var aegis_allocator_t::free
 *   Free function. Must be safe to call with ptr == NULL.
 * @var aegis_allocator_t::realloc
 *   Reallocate function. old_size and new_size are hints; preserving
 *   content up to min(old_size, new_size) is required.
 * @var aegis_allocator_t::stats
 *   Stats function; may be NULL.
 * @var aegis_allocator_t::destroy
 *   Destroy function; may be NULL (caller must not call it).
 * @var aegis_allocator_t::ctx
 *   Opaque context passed through to every vtable callback.
 */
struct aegis_allocator {
    aegis_alloc_fn   alloc;
    aegis_free_fn    free;
    aegis_realloc_fn realloc;
    aegis_stats_fn   stats;
    aegis_destroy_fn destroy;
    void*            ctx; /**< Passed through to every callback. */
};

/* ── Default (system) allocator ────────────────────────────────────────────── */

/**
 * @brief Return the singleton default (system) allocator.
 *
 * Read-only; do not modify the returned structure.
 * Lifetime is process-wide.
 */
const aegis_allocator_t* aegis_alloc_default(void);

/**
 * @brief Allocate size bytes using the given allocator.
 *
 * If @p alloc is NULL, falls back to the system malloc.
 *
 * @param alloc  Allocator to use (borrowed; may be NULL).
 * @param size   Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void* aegis_alloc(const aegis_allocator_t* alloc, size_t size);

/**
 * @brief Free a block previously allocated by the given allocator.
 *
 * Safe to call with NULL ptr or NULL allocator (delegates to free()).
 *
 * @param alloc  Allocator that produced the pointer (may be NULL).
 * @param ptr    Block to free (may be NULL — no-op).
 */
void aegis_free(const aegis_allocator_t* alloc, void* ptr);

/**
 * @brief Reallocate a block to new_size bytes.
 *
 * Preserves content up to min(old_size, new_size).
 *
 * @param alloc    Allocator to use (may be NULL → system realloc).
 * @param ptr      Block to resize (may be NULL → behaves like alloc).
 * @param old_size Size of the current block (ignored if ptr is NULL).
 * @param new_size Desired new size.
 * @return Pointer to resized block, or NULL on failure (original block unchanged).
 */
void* aegis_realloc(const aegis_allocator_t* alloc, void* ptr,
                    size_t old_size, size_t new_size);

/**
 * @brief Query allocation statistics from the given allocator.
 *
 * If @p stats is NULL the call is a no-op.
 * If @p alloc is NULL, queries the default allocator.
 *
 * @param alloc  Allocator to query (may be NULL).
 * @param stats  Output buffer for stats (may be NULL).
 */
void aegis_alloc_stats(const aegis_allocator_t* alloc, aegis_alloc_stats_t* stats);

/* ── Tracking allocator (wraps another allocator) ──────────────────────────── */

/**
 * @brief Create a tracking allocator that wraps @p base.
 *
 * The returned allocator counts allocations/deallocations and
 * reports cumulative byte counts via aegis_alloc_stats().
 *
 * @param base  Allocator to wrap (borrowed; must outlive the tracker).
 *              Pass NULL to use the system default as the underlying allocator.
 * @return A tracking allocator on success; all fields are zeroed on failure.
 */
aegis_allocator_t aegis_alloc_tracker(const aegis_allocator_t* base);

/**
 * @brief Destroy a tracking allocator and free its internal context.
 *
 * Call this when the tracking allocator is no longer needed to avoid
 * leaking the tracking context struct. Safe to call with a zeroed
 * allocator (no-op).
 *
 * @param tracker The tracking allocator to destroy (by value).
 * @return Zeroed allocator.
 */
aegis_allocator_t aegis_alloc_tracker_destroy(aegis_allocator_t tracker);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ALLOCATOR_H */
