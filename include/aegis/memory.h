/**
 * @file memory.h
 * @brief Memory abstractions: item, store, and typed memory types.
 *
 * Memory is decoupled from any particular Storage backend. It operates
 * entirely in-process using the common vector/list primitives. Storage
 * persistence (SQLite, etc.) can be layered on top later via a
 * pluggable backend if needed.
 *
 * Memory items are owned by the memory store once inserted. The store
 * makes its own copies of all strings so callers may free or reuse
 * their buffers immediately after insertion.
 *
 * Thread safety: individual memory stores are NOT thread-safe. Callers
 * who access a store from multiple threads must synchronize externally.
 */
#ifndef AEGIS_MEMORY_H
#define AEGIS_MEMORY_H

#include "aegis/status.h"
#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Memory item ───────────────────────────────────────────────────────────── */

/** Memory item type classification. */
typedef enum aegis_memory_item_type {
    AEGIS_MEMORY_ITEM_GENERIC,     /**< Unspecified content.                */
    AEGIS_MEMORY_ITEM_GOAL,        /**< A goal or objective.                */
    AEGIS_MEMORY_ITEM_PLAN,        /**< A plan or sub-plan.                 */
    AEGIS_MEMORY_ITEM_TASK_RESULT, /**< Result of a task execution.         */
    AEGIS_MEMORY_ITEM_OBSERVATION, /**< An observation from the environment. */
    AEGIS_MEMORY_ITEM_KNOWLEDGE,   /**< Semantic knowledge fact.            */
    AEGIS_MEMORY_ITEM_EXPERIENCE,  /**< Procedural experience / lesson.    */
} aegis_memory_item_type_t;

/**
 * @brief One memory item.
 *
 * All strings are copied into the item on creation (except @p metadata
 * keys/values which are borrowed until the item is destroyed).
 */
typedef struct aegis_memory_item {
    char*                    id;      /**< Owned UUID-like identifier.      */
    char*                    content; /**< Owned text content.               */
    aegis_memory_item_type_t type;
    uint64_t                 timestamp;     /**< Milliseconds since epoch.         */
    int                      priority;      /**< Higher = more important.          */
    const char**             metadata_keys; /**< Borrowed. May be NULL.         */
    const char**             metadata_vals; /**< Borrowed. May be NULL.         */
    size_t                   n_metadata;    /**< Length of above arrays.          */
} aegis_memory_item_t;

/* ── Generic memory store ──────────────────────────────────────────────────── */

/** Opaque memory store handle. */
typedef struct aegis_memory aegis_memory_t;

/**
 * @brief Create an empty memory store.
 *
 * @param[out] out   Receives the store. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_memory_create(aegis_memory_t** out);

/**
 * @brief Destroy a memory store and free all items it owns.
 *
 * Safe to call with NULL (no-op).
 *
 * @param mem Store handle (ownership: consumed).
 */
void aegis_memory_destroy(aegis_memory_t* mem);

/** Number of items in the store. NULL → 0. */
size_t aegis_memory_count(const aegis_memory_t* mem);

/**
 * @brief Insert an item into the store.
 *
 * The store takes ownership of the item; the caller must NOT free
 * @p item after this call. The store copies all strings inside
 * @p item.
 *
 * @param mem  Store (borrowed).
 * @param item Item to insert (ownership: transferred).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_memory_put(aegis_memory_t* mem, aegis_memory_item_t* item);

/**
 * @brief Look up an item by id.
 *
 * @param mem  Store (borrowed).
 * @param id   Item id (borrowed; required).
 * @param[out] out Receives the item pointer. Ownership: BORROWED —
 *                 valid until the store is destroyed or the item is removed.
 * @return AEGIS_OK on success, AEGIS_ERR_NOT_FOUND when absent.
 */
aegis_status_t aegis_memory_get(const aegis_memory_t* mem, const char* id,
                                aegis_memory_item_t** out);

/**
 * @brief Search items by type.
 *
 * @param mem      Store (borrowed).
 * @param type     Item type to match (AEGIS_MEMORY_ITEM_GENERIC matches all).
 * @param[out] out Receives a malloc'd array of item pointers. Ownership:
 *                 transferred to caller; caller must free() the array itself
 *                 (items remain owned by the store).
 * @param[out] out_count Receives the number of matched items.
 * @return AEGIS_OK.
 */
aegis_status_t aegis_memory_search_by_type(const aegis_memory_t* mem, aegis_memory_item_type_t type,
                                           aegis_memory_item_t*** out, size_t* out_count);

/**
 * @brief Remove an item by id.
 *
 * The removed item is transferred to the caller (ownership: transferred).
 * The caller must call aegis_memory_item_destroy() on the returned item.
 *
 * @param mem  Store (borrowed).
 * @param id   Item id (borrowed; required).
 * @param[out] out Receives the removed item. May be NULL (item silently dropped).
 * @return AEGIS_OK on success, AEGIS_ERR_NOT_FOUND when absent.
 */
aegis_status_t aegis_memory_remove(aegis_memory_t* mem, const char* id, aegis_memory_item_t** out);

/**
 * @brief Destroy a memory item and free all owned strings.
 *
 * Safe to call with NULL.
 *
 * @param item Item handle (ownership: consumed).
 */
void aegis_memory_item_destroy(aegis_memory_item_t* item);

/* ── Typed memory handles ──────────────────────────────────────────────────── */

/** Opaque typed memory handles. Layouts are in memory_internal.h. */
typedef struct aegis_working_memory    aegis_working_memory_t;
typedef struct aegis_episodic_memory   aegis_episodic_memory_t;
typedef struct aegis_semantic_memory   aegis_semantic_memory_t;
typedef struct aegis_procedural_memory aegis_procedural_memory_t;

/* ── Working memory ────────────────────────────────────────────────────────── */

/**
 * @brief Create a working memory store with a capacity limit.
 *
 * @param[out] out       Receives the store. Ownership: transferred.
 * @param max_capacity   Hard cap on items; oldest low-priority items are
 *                       evicted on overflow (0 = no cap).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_working_memory_create(aegis_working_memory_t** out, size_t max_capacity);

/** Destroy a working memory. Safe to call with NULL. */
void aegis_working_memory_destroy(aegis_working_memory_t* mem);

size_t aegis_working_memory_count(const aegis_working_memory_t* mem);

/**
 * @brief Insert an item. On overflow, the lowest-priority item is evicted.
 *
 * @param mem  Store (borrowed).
 * @param item Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_working_memory_put(aegis_working_memory_t* mem, aegis_memory_item_t* item);

/**
 * @brief Get top-N highest-priority items (most recent first within same priority).
 *
 * @param mem     Store (borrowed).
 * @param n       Max items to return.
 * @param[out] out Receives a malloc'd array of item pointers. Ownership:
 *                 transferred; caller must free() the array. Items remain owned
 *                 by the store.
 * @param[out] out_count Receives actual count returned.
 * @return AEGIS_OK.
 */
aegis_status_t aegis_working_memory_top(const aegis_working_memory_t* mem, size_t n,
                                        aegis_memory_item_t*** out, size_t* out_count);

/* ── Episodic memory ───────────────────────────────────────────────────────── */

/**
 * @brief Create an episodic memory store.
 *
 * @param[out] out  Receives the store. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_episodic_memory_create(aegis_episodic_memory_t** out);

/** Destroy an episodic memory. Safe to call with NULL. */
void aegis_episodic_memory_destroy(aegis_episodic_memory_t* mem);

size_t aegis_episodic_memory_count(const aegis_episodic_memory_t* mem);

/**
 * @brief Append an event record.
 *
 * @param mem  Store (borrowed).
 * @param item Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_episodic_memory_append(aegis_episodic_memory_t* mem,
                                            aegis_memory_item_t*     item);

/**
 * @brief Retrieve events in a time range [start_ms, end_ms).
 *
 * @param mem      Store (borrowed).
 * @param start_ms Inclusive start timestamp.
 * @param end_ms   Exclusive end timestamp.
 * @param[out] out Receives a malloc'd array. Ownership: transferred.
 * @param[out] out_count Receives count.
 * @return AEGIS_OK.
 */
aegis_status_t aegis_episodic_memory_range(const aegis_episodic_memory_t* mem, uint64_t start_ms,
                                           uint64_t end_ms, aegis_memory_item_t*** out,
                                           size_t* out_count);

/* ── Semantic memory ───────────────────────────────────────────────────────── */

/**
 * @brief Create a semantic memory store.
 *
 * @param[out] out  Receives the store. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_semantic_memory_create(aegis_semantic_memory_t** out);

/** Destroy a semantic memory. Safe to call with NULL. */
void aegis_semantic_memory_destroy(aegis_semantic_memory_t* mem);

size_t aegis_semantic_memory_count(const aegis_semantic_memory_t* mem);

/**
 * @brief Insert a knowledge fact. Duplicate ids overwrite the old fact.
 *
 * @param mem  Store (borrowed).
 * @param item Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_semantic_memory_put(aegis_semantic_memory_t* mem, aegis_memory_item_t* item);

/**
 * @brief Lookup by id.
 *
 * @param mem  Store (borrowed).
 * @param id   Fact id (borrowed).
 * @param[out] out Receives pointer. Ownership: BORROWED.
 * @return AEGIS_OK or AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_semantic_memory_get(const aegis_semantic_memory_t* mem, const char* id,
                                         aegis_memory_item_t** out);

/* ── Procedural memory ─────────────────────────────────────────────────────── */

/**
 * @brief Create a procedural memory store.
 *
 * @param[out] out  Receives the store. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_procedural_memory_create(aegis_procedural_memory_t** out);

/** Destroy a procedural memory. Safe to call with NULL. */
void aegis_procedural_memory_destroy(aegis_procedural_memory_t* mem);

size_t aegis_procedural_memory_count(const aegis_procedural_memory_t* mem);

/**
 * @brief Record an experience entry.
 *
 * @param mem  Store (borrowed).
 * @param item Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_procedural_memory_put(aegis_procedural_memory_t* mem,
                                           aegis_memory_item_t*       item);

/**
 * @brief Search by keyword substring match in content.
 *
 * @param mem      Store (borrowed).
 * @param keyword  Substring to search for (borrowed; required).
 * @param[out] out  Receives malloc'd array. Ownership: transferred.
 * @param[out] out_count Receives count.
 * @return AEGIS_OK.
 */
aegis_status_t aegis_procedural_memory_search(const aegis_procedural_memory_t* mem,
                                              const char* keyword, aegis_memory_item_t*** out,
                                              size_t* out_count);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MEMORY_H */
