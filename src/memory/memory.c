/**
 * @file memory.c
 * @brief Generic in-process memory store plus shared item helpers.
 *
 * Vector-backed item storage with cloned-string ownership; backs the
 * typed working/episodic/semantic/procedural stores together with the
 * item clone/free helpers. Individual stores are NOT thread-safe.
 */

#define _POSIX_C_SOURCE 200809L
#include "aegis/memory/memory.h"
#include "aegis/status.h"

#include "memory_internal.h"
#include "memory_helpers.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Create an empty in-process memory container.
 *
 * @param[out] out Receives the new container; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_memory_create(aegis_memory_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_memory_t* mem = calloc(1, sizeof(*mem));
    if (!mem) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&mem->items, sizeof(aegis_memory_item_t*));
    if (rc != 0) {
        free(mem);
        return AEGIS_ERR_NOMEM;
    }
    *out = mem;
    return AEGIS_OK;
}

/**
 * @brief Destroy a memory container, freeing every item it holds.
 *
 * NULL is a no-op. All contained aegis_memory_item_t pointers are
 * deallocated via the item's own free function (if set).
 *
 * @param[in] mem Container to destroy, or NULL.
 */
void aegis_memory_destroy(aegis_memory_t* mem)
{
    if (!mem) {
        return;
    }
    if (mem->items) {
        size_t n = aegis_vector_len(mem->items);
        for (size_t i = 0; i < n; i++) {
            aegis_memory_item_t* item = NULL;
            aegis_vector_get(mem->items, i, &item);
            free_item(item);
        }
        aegis_vector_destroy(mem->items);
    }
    free(mem);
}

/**
 * @brief Return the number of items stored.
 *
 * @param[in] mem Container, or NULL.
 * @return Item count, or 0 for NULL.
 */
size_t aegis_memory_count(const aegis_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Insert a cloned copy of an item under its id.
 *
 * Validates @p item (non-empty id and non-NULL content) and inserts a
 * deep clone. Ownership of the clone is transferred to the container.
 * The original item is NOT modified or consumed. Not thread-safe.
 *
 * @param[in] mem  Container to extend.
 * @param[in] item Item to clone and insert.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_memory_put(aegis_memory_t* mem, aegis_memory_item_t* item)
{
    if (!mem || !item) {
        return AEGIS_ERR_INVALID;
    }
    if (!item->id || item->id[0] == '\0' || !item->content) {
        return AEGIS_ERR_INVALID;
    }
    aegis_memory_item_t* cloned = clone_item(item);
    if (!cloned) {
        return AEGIS_ERR_NOMEM;
    }
    /* Overwrite existing item with same id. */
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* existing = NULL;
        aegis_vector_get(mem->items, i, &existing);
        if (existing && strcmp(existing->id, item->id) == 0) {
            free_item(existing);
            int rc = aegis_vector_set(mem->items, i, &cloned);
            (void)rc;
            free_item(item);
            return AEGIS_OK;
        }
    }
    int rc = aegis_vector_push(mem->items, &cloned);
    if (rc != 0) {
        free_item(cloned);
        return AEGIS_ERR_NOMEM;
    }
    free_item(item);
    return AEGIS_OK;
}

/**
 * @brief Borrow an item by id without removing it.
 *
 * Linear scan; valid until the container is mutated. Returns
 * AEGIS_ERR_NOT_FOUND when the id is absent.
 *
 * @param[in]  mem  Container to query.
 * @param[in]  id   Item id to find.
 * @param[out] out  Receives a borrowed pointer (do NOT free); untouched on
 *                  not-found.
 * @return AEGIS_OK on success (including not-found with @p out=NULL),
 *   AEGIS_ERR_INVALID for NULL args.
 */
aegis_status_t aegis_memory_get(const aegis_memory_t* mem, const char* id,
                                aegis_memory_item_t** out)
{
    if (!mem || !id || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out     = NULL;
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && strcmp(item->id, id) == 0) {
            *out = item;
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}

/**
 * @brief Return all items of the given type, allocatuing a caller-owned
 *        array that must be freed with free(*out).
 *
 * Pass AEGIS_MEMORY_ITEM_GENERIC to collect all items regardless of type.
 * The returned array pointers are borrowed from the container — the caller
 * must not free the individual items, only the array itself via free().
 * Not thread-safe.
 *
 * @param[in]  mem       Container to query.
 * @param[in]  type      Item type to filter by, or AEGIS_MEMORY_ITEM_GENERIC.
 * @param[out] out       Receives the caller-owned array of item pointers.
 * @param[out] out_count Receives the number of returned items.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_memory_search_by_type(const aegis_memory_t* mem, aegis_memory_item_type_t type,
                                           aegis_memory_item_t*** out, size_t* out_count)
{
    if (!mem || !out || !out_count) {
        return AEGIS_ERR_INVALID;
    }
    *out       = NULL;
    *out_count = 0;
    size_t n   = aegis_vector_len(mem->items);
    if (n == 0) {
        return AEGIS_OK;
    }
    size_t match_count = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (!item) {
            continue;
        }
        if (type == AEGIS_MEMORY_ITEM_GENERIC || item->type == type) {
            match_count++;
        }
    }
    if (match_count == 0) {
        return AEGIS_OK;
    }
    *out = malloc(sizeof(aegis_memory_item_t*) * match_count);
    if (!*out) {
        return AEGIS_ERR_NOMEM;
    }
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (!item) {
            continue;
        }
        if (type == AEGIS_MEMORY_ITEM_GENERIC || item->type == type) {
            (*out)[idx++] = item;
        }
    }
    *out_count = idx;
    return AEGIS_OK;
}

/**
 * @brief Remove an item by id, optionally returning it.
 *
 * When @p out is non-NULL the caller takes ownership of the removed item;
 * otherwise the item is freed in-place. Array shift preserves order; the
 * vacated tail slot is zeroed before pop to avoid dangling pointers. Not
 * thread-safe.
 *
 * @param[in]  mem  Container to shrink.
 * @param[in]  id   Item id to remove.
 * @param[out] out  Receives the removed item (borrowed from the caller's
 *                  perspective), or NULL to drop/free the item.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOT_FOUND when the id is absent.
 */
aegis_status_t aegis_memory_remove(aegis_memory_t* mem, const char* id, aegis_memory_item_t** out)
{
    if (!mem || !id) {
        return AEGIS_ERR_INVALID;
    }
    if (out) {
        *out = NULL;
    }
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && strcmp(item->id, id) == 0) {
            /* Shift elements after i down by one, then pop the tail.
             * Save evict BEFORE the shift. After shifting, zero the
             * vacated tail slot to prevent a dangling pointer. */
            aegis_memory_item_t* evict = item;
            for (size_t j = i; j + 1 < n; j++) {
                aegis_memory_item_t* nxt = NULL;
                aegis_vector_get(mem->items, j + 1, &nxt);
                aegis_vector_set(mem->items, j, &nxt);
            }
            /* Zero the vacated tail slot before pop writes into it. */
            aegis_memory_item_t null_item = {0};
            aegis_vector_set(mem->items, n - 1, &null_item);
            aegis_memory_item_t* drop = NULL;
            aegis_vector_pop(mem->items, &drop);
            (void)drop;
            if (out) {
                *out = evict;
            } else {
                free_item(evict);
            }
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}

/**
 * @brief Destroy a memory item via its free function (NULL-safe).
 *
 * This wraps the dynamic free_item for callers who received an item
 * pointer and wish to release it rather than remove it from a container.
 *
 * @param[in] item Item to free, or NULL.
 */
void aegis_memory_item_destroy(aegis_memory_item_t* item)
{
    free_item(item);
}

/* ── Working memory ────────────────────────────────────────────────────────── */

/**
 * @brief Create a working-memory store with the given capacity ceiling.
 *
 * Working memory evicts the lowest-priority item when putting an item
 * would exceed @p max_capacity. Callers retain ownership of their item
 * pointers — a deep clone is stored internally.
 *
 * @param[out] out            Receives the new store; untouched on failure.
 * @param[in]  max_capacity   Hard cap on concurrent items (0 means unlimited).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_working_memory_create(aegis_working_memory_t** out, size_t max_capacity)
{
    AEGIS_CHECK_OUT(out);
    aegis_working_memory_t* mem = calloc(1, sizeof(*mem));
    if (!mem) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&mem->items, sizeof(aegis_memory_item_t*));
    if (rc != 0) {
        free(mem);
        return AEGIS_ERR_NOMEM;
    }
    mem->max_capacity = max_capacity;
    *out              = mem;
    return AEGIS_OK;
}

/**
 * @brief Destroy a working-memory store and all items it holds.
 *
 * NULL is a no-op.
 *
 * @param[in] mem Working-memory store to destroy, or NULL.
 */
void aegis_working_memory_destroy(aegis_working_memory_t* mem)
{
    if (!mem) {
        return;
    }
    if (mem->items) {
        size_t n = aegis_vector_len(mem->items);
        for (size_t i = 0; i < n; i++) {
            aegis_memory_item_t* item = NULL;
            aegis_vector_get(mem->items, i, &item);
            free_item(item);
        }
        aegis_vector_destroy(mem->items);
    }
    free(mem);
}

/**
 * @brief Return the number of items currently in the working store.
 *
 * @param[in] mem Working-memory store, or NULL.
 * @return Item count, or 0 for NULL.
 */
size_t aegis_working_memory_count(const aegis_working_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Insert a cloned item, evicting the lowest-priority item if the
 *        store is already at capacity.
 *
 * Items below the configured max_capacity are accepted immediately. When
 * full, the item with the smallest priority value is dropped (freed) to
 * make room for the new item. Not thread-safe.
 *
 * @param[in] mem  Working-memory store to extend.
 * @param[in] item Item to insert (deep-cloned; original unchanged).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_working_memory_put(aegis_working_memory_t* mem, aegis_memory_item_t* item)
{
    if (!mem || !item) {
        return AEGIS_ERR_INVALID;
    }
    if (!item->id || item->id[0] == '\0' || !item->content) {
        return AEGIS_ERR_INVALID;
    }
    aegis_memory_item_t* cloned = clone_item(item);
    if (!cloned) {
        return AEGIS_ERR_NOMEM;
    }
    /* Overwrite if same id exists. */
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* existing = NULL;
        aegis_vector_get(mem->items, i, &existing);
        if (existing && strcmp(existing->id, item->id) == 0) {
            free_item(existing);
            int rc = aegis_vector_set(mem->items, i, &cloned);
            (void)rc;
            free_item(item);
            return AEGIS_OK;
        }
    }
    int rc = aegis_vector_push(mem->items, &cloned);
    if (rc != 0) {
        free_item(cloned);
        return AEGIS_ERR_NOMEM;
    }
    free_item(item);
    /* Evict lowest-priority item if over capacity. */
    if (mem->max_capacity > 0) {
        n = aegis_vector_len(mem->items);
        while (n > mem->max_capacity) {
            size_t               lowest_idx = 0;
            aegis_memory_item_t* lowest     = NULL;
            aegis_vector_get(mem->items, 0, &lowest);
            for (size_t i = 1; i < n; i++) {
                aegis_memory_item_t* cur = NULL;
                aegis_vector_get(mem->items, i, &cur);
                if (cur && (!lowest || cur->priority < lowest->priority)) {
                    lowest     = cur;
                    lowest_idx = i;
                }
            }
            if (!lowest) {
                break;
            }
            /* Shift items after lowest_idx down by one, then pop. */
            aegis_memory_item_t* evict = NULL;
            aegis_vector_get(mem->items, lowest_idx, &evict);
            for (size_t j = lowest_idx; j + 1 < n; j++) {
                aegis_memory_item_t* nxt = NULL;
                aegis_vector_get(mem->items, j + 1, &nxt);
                aegis_vector_set(mem->items, j, &nxt);
            }
            aegis_memory_item_t* drop = NULL;
            aegis_vector_pop(mem->items, &drop);
            (void)drop; /* should be same as evict */
            free_item(evict);
            n--;
        }
    }
    return AEGIS_OK;
}

/**
 * @brief Borrow the N highest-priority items (stable descending order).
 *
 * Sorts by priority descending (bubble sort; stores are small) and fills
 * @p out with at most @p n pointers. The returned pointers are borrowed
 * from the store — do not free them. Not thread-safe.
 *
 * @param[in]  mem      Working-memory store.
 * @param[in]  n        Maximum number of top items to return.
 * @param[out] out      Caller-owned array of at least @p n pointers.
 * @param[out] out_count Receives the actual number returned (< = @p n).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_working_memory_top(const aegis_working_memory_t* mem, size_t n,
                                        aegis_memory_item_t*** out, size_t* out_count)
{
    if (!mem || !out || !out_count) {
        return AEGIS_ERR_INVALID;
    }
    *out         = NULL;
    *out_count   = 0;
    size_t total = aegis_vector_len(mem->items);
    if (total == 0) {
        return AEGIS_OK;
    }
    size_t count = (n < total) ? n : total;
    *out         = malloc(sizeof(aegis_memory_item_t*) * count);
    if (!*out) {
        return AEGIS_ERR_NOMEM;
    }
    aegis_memory_item_t** sorted = malloc(sizeof(aegis_memory_item_t*) * total);
    if (!sorted) {
        free(*out);
        *out = NULL;
        return AEGIS_ERR_NOMEM;
    }
    for (size_t i = 0; i < total; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        sorted[i] = item;
    }
    /* Bubble sort descending by priority (total is small). */
    for (size_t i = 0; i < total; i++) {
        for (size_t j = i + 1; j < total; j++) {
            if (cmp_item_by_priority_desc(&sorted[i], &sorted[j]) > 0) {
                aegis_memory_item_t* tmp = sorted[i];
                sorted[i]                = sorted[j];
                sorted[j]                = tmp;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        (*out)[i] = sorted[i];
    }
    free(sorted);
    *out_count = count;
    return AEGIS_OK;
}

/* ── Episodic memory ───────────────────────────────────────────────────────── */

/**
 * @brief Create an append-only episodic store.
 *
 * Items are stored in insertion order; range queries are half-open
 * [start_ms, end_ms). Not thread-safe.
 *
 * @param[out] out Receives the new store; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_episodic_memory_create(aegis_episodic_memory_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_episodic_memory_t* mem = calloc(1, sizeof(*mem));
    if (!mem) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&mem->items, sizeof(aegis_memory_item_t*));
    if (rc != 0) {
        free(mem);
        return AEGIS_ERR_NOMEM;
    }
    *out = mem;
    return AEGIS_OK;
}

/**
 * @brief Destroy an episodic store, freeing every item it holds.
 *
 * NULL is a no-op.
 *
 * @param[in] mem Episodic store to destroy, or NULL.
 */
void aegis_episodic_memory_destroy(aegis_episodic_memory_t* mem)
{
    if (!mem) {
        return;
    }
    if (mem->items) {
        size_t n = aegis_vector_len(mem->items);
        for (size_t i = 0; i < n; i++) {
            aegis_memory_item_t* item = NULL;
            aegis_vector_get(mem->items, i, &item);
            free_item(item);
        }
        aegis_vector_destroy(mem->items);
    }
    free(mem);
}

/**
 * @brief Return the number of records in the store.
 *
 * @param[in] mem Episodic store, or NULL.
 * @return Record count, or 0 for NULL.
 */
size_t aegis_episodic_memory_count(const aegis_episodic_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Append a cloned item to the end of the record list.
 *
 * Ownership of the clone transfers to the store. The original item is
 * unchanged. Not thread-safe.
 *
 * @param[in] mem  Episodic store to extend.
 * @param[in] item Item to append (deep-cloned; original unchanged).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_episodic_memory_append(aegis_episodic_memory_t* mem, aegis_memory_item_t* item)
{
    if (!mem || !item) {
        return AEGIS_ERR_INVALID;
    }
    if (!item->id || item->id[0] == '\0' || !item->content) {
        return AEGIS_ERR_INVALID;
    }
    aegis_memory_item_t* cloned = clone_item(item);
    if (!cloned) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_push(mem->items, &cloned);
    if (rc != 0) {
        free_item(cloned);
        return AEGIS_ERR_NOMEM;
    }
    free_item(item);
    return AEGIS_OK;
}

/**
 * @brief Return the number of items in a half-open time range
 *        [start_ms, end_ms).
 *
 * Items outside the range are skipped; the return value is the count of
 * matching entries only. Not thread-safe.
 *
 * @param[in]  mem      Episodic store.
 * @param[in]  start_ms Inclusive start, or 0 for "from the beginning".
 * @param[in]  end_ms   Exclusive end, or 0 for "to the end".
 * @param[out] out      Caller-owned array of matching item pointers (do
 *                      NOT free the pointed items).
 * @param[out] out_count Receives the number of returned items.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_episodic_memory_range(const aegis_episodic_memory_t* mem, uint64_t start_ms,
                                           uint64_t end_ms, aegis_memory_item_t*** out,
                                           size_t* out_count)
{
    if (!mem || !out || !out_count) {
        return AEGIS_ERR_INVALID;
    }
    *out       = NULL;
    *out_count = 0;
    size_t n   = aegis_vector_len(mem->items);
    if (n == 0) {
        return AEGIS_OK;
    }
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && item->timestamp >= start_ms && item->timestamp < end_ms) {
            count++;
        }
    }
    if (count == 0) {
        return AEGIS_OK;
    }
    *out = malloc(sizeof(aegis_memory_item_t*) * count);
    if (!*out) {
        return AEGIS_ERR_NOMEM;
    }
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && item->timestamp >= start_ms && item->timestamp < end_ms) {
            (*out)[idx++] = item;
        }
    }
    *out_count = idx;
    return AEGIS_OK;
}

/* ── Semantic memory ───────────────────────────────────────────────────────── */

/**
 * @brief Create an id-keyed semantic store.
 *
 * Insert is by id (overwrites on duplicate); lookup is a linear scan.
 * Not thread-safe.
 *
 * @param[out] out Receives the new store; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_semantic_memory_create(aegis_semantic_memory_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_semantic_memory_t* mem = calloc(1, sizeof(*mem));
    if (!mem) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&mem->items, sizeof(aegis_memory_item_t*));
    if (rc != 0) {
        free(mem);
        return AEGIS_ERR_NOMEM;
    }
    *out = mem;
    return AEGIS_OK;
}

/**
 * @brief Destroy a semantic store, freeing every item it holds.
 *
 * NULL is a no-op.
 *
 * @param[in] mem Semantic store to destroy, or NULL.
 */
void aegis_semantic_memory_destroy(aegis_semantic_memory_t* mem)
{
    if (!mem) {
        return;
    }
    if (mem->items) {
        size_t n = aegis_vector_len(mem->items);
        for (size_t i = 0; i < n; i++) {
            aegis_memory_item_t* item = NULL;
            aegis_vector_get(mem->items, i, &item);
            free_item(item);
        }
        aegis_vector_destroy(mem->items);
    }
    free(mem);
}

/**
 * @brief Return the number of entries in the store.
 *
 * @param[in] mem Semantic store, or NULL.
 * @return Entry count, or 0 for NULL.
 */
size_t aegis_semantic_memory_count(const aegis_semantic_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Insert or overwrite a cloned item by id.
 *
 * When an item with the same id already exists it is replaced (the old
 * item is freed). The new item is deep-cloned before storage; the
 * original is unchanged. Not thread-safe.
 *
 * @param[in] mem  Semantic store to extend.
 * @param[in] item Item to insert (deep-cloned; original unchanged).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_semantic_memory_put(aegis_semantic_memory_t* mem, aegis_memory_item_t* item)
{
    if (!mem || !item) {
        return AEGIS_ERR_INVALID;
    }
    if (!item->id || item->id[0] == '\0' || !item->content) {
        return AEGIS_ERR_INVALID;
    }
    aegis_memory_item_t* cloned = clone_item(item);
    if (!cloned) {
        return AEGIS_ERR_NOMEM;
    }
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* existing = NULL;
        aegis_vector_get(mem->items, i, &existing);
        if (existing && strcmp(existing->id, item->id) == 0) {
            free_item(existing);
            int rc = aegis_vector_set(mem->items, i, &cloned);
            (void)rc;
            free_item(item);
            return AEGIS_OK;
        }
    }
    int rc = aegis_vector_push(mem->items, &cloned);
    if (rc != 0) {
        free_item(cloned);
        return AEGIS_ERR_NOMEM;
    }
    free_item(item);
    return AEGIS_OK;
}

/**
 * @brief Borrow a single item by id.
 *
 * Linear scan; valid until the store is mutated. Returns
 * AEGIS_ERR_NOT_FOUND when the id is absent. Not thread-safe.
 *
 * @param[in]  mem  Semantic store to query.
 * @param[in]  id   Item id to find.
 * @param[out] out  Receives a borrowed pointer (do NOT free); untouched on
 *                  not-found.
 * @return AEGIS_OK on success (including not-found with @p out=NULL),
 *   AEGIS_ERR_INVALID for NULL args.
 */
aegis_status_t aegis_semantic_memory_get(const aegis_semantic_memory_t* mem, const char* id,
                                         aegis_memory_item_t** out)
{
    if (!mem || !id || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out     = NULL;
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && strcmp(item->id, id) == 0) {
            *out = item;
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}

/* ── Procedural memory ─────────────────────────────────────────────────────── */

aegis_status_t aegis_procedural_memory_create(aegis_procedural_memory_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_procedural_memory_t* mem = calloc(1, sizeof(*mem));
    if (!mem) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&mem->items, sizeof(aegis_memory_item_t*));
    if (rc != 0) {
        free(mem);
        return AEGIS_ERR_NOMEM;
    }
    *out = mem;
    return AEGIS_OK;
}

void aegis_procedural_memory_destroy(aegis_procedural_memory_t* mem)
{
    if (!mem) {
        return;
    }
    if (mem->items) {
        size_t n = aegis_vector_len(mem->items);
        for (size_t i = 0; i < n; i++) {
            aegis_memory_item_t* item = NULL;
            aegis_vector_get(mem->items, i, &item);
            free_item(item);
        }
        aegis_vector_destroy(mem->items);
    }
    free(mem);
}

size_t aegis_procedural_memory_count(const aegis_procedural_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

aegis_status_t aegis_procedural_memory_put(aegis_procedural_memory_t* mem,
                                           aegis_memory_item_t*       item)
{
    if (!mem || !item) {
        return AEGIS_ERR_INVALID;
    }
    if (!item->id || item->id[0] == '\0' || !item->content) {
        return AEGIS_ERR_INVALID;
    }
    aegis_memory_item_t* cloned = clone_item(item);
    if (!cloned) {
        return AEGIS_ERR_NOMEM;
    }
    size_t n = aegis_vector_len(mem->items);
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* existing = NULL;
        aegis_vector_get(mem->items, i, &existing);
        if (existing && strcmp(existing->id, item->id) == 0) {
            free_item(existing);
            int rc = aegis_vector_set(mem->items, i, &cloned);
            (void)rc;
            free_item(item);
            return AEGIS_OK;
        }
    }
    int rc = aegis_vector_push(mem->items, &cloned);
    if (rc != 0) {
        free_item(cloned);
        return AEGIS_ERR_NOMEM;
    }
    free_item(item);
    return AEGIS_OK;
}

aegis_status_t aegis_procedural_memory_search(const aegis_procedural_memory_t* mem,
                                              const char* keyword, aegis_memory_item_t*** out,
                                              size_t* out_count)
{
    if (!mem || !keyword || !out || !out_count) {
        return AEGIS_ERR_INVALID;
    }
    *out       = NULL;
    *out_count = 0;
    size_t n   = aegis_vector_len(mem->items);
    if (n == 0) {
        return AEGIS_OK;
    }
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && strstr(item->content, keyword) != NULL) {
            count++;
        }
    }
    if (count == 0) {
        return AEGIS_OK;
    }
    *out = malloc(sizeof(aegis_memory_item_t*) * count);
    if (!*out) {
        return AEGIS_ERR_NOMEM;
    }
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        aegis_memory_item_t* item = NULL;
        aegis_vector_get(mem->items, i, &item);
        if (item && strstr(item->content, keyword) != NULL) {
            (*out)[idx++] = item;
        }
    }
    *out_count = idx;
    return AEGIS_OK;
}
