/**
 * @file memory_episodic.c
 * @brief Episodic memory: append-only event records with time-range
 * retrieval. Items are owned copies; ranges are half-open [start, end).
 */
/* ── Episodic memory ───────────────────────────────────────────────────────── */
#define _POSIX_C_SOURCE 200809L
#include "aegis/memory/memory.h"
#include "aegis/status.h"

#include "memory_internal.h"
#include "memory_helpers.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdlib.h>
#include <string.h>

/* ── Episodic memory ───────────────────────────────────────────────────────── */

/**
 * @brief Create an empty episodic memory store.
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
 * @brief Destroy an episodic memory store, freeing all items.
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
 * @brief Return the number of items in the episodic memory.
 */
size_t aegis_episodic_memory_count(const aegis_episodic_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Append an item to episodic memory. The item is consumed on success.
 *
 * @param[in]  mem   Episodic memory store.
 * @param[in]  item  Item to append (ownership transferred on success).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args or empty
 *         id/content, AEGIS_ERR_NOMEM on allocation failure.
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
 * @brief Retrieve items in a half-open time range [start_ms, end_ms).
 *
 * @param[in]  mem       Episodic memory store.
 * @param[in]  start_ms  Inclusive start (ms since epoch).
 * @param[in]  end_ms    Exclusive end (ms since epoch).
 * @param[out] out       Heap-allocated array of item pointers (caller frees).
 * @param[out] out_count  Number of items in range.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *         AEGIS_ERR_NOMEM on allocation failure.
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
