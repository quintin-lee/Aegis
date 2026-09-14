/**
 * @file memory_working.c
 * @brief Working memory: capacity-capped store evicting lowest-priority
 * items on overflow, with top-N highest-priority retrieval.
 */
/* ── Working memory ────────────────────────────────────────────────────────── */
#define _POSIX_C_SOURCE 200809L
#include "aegis/memory/memory.h"
#include "aegis/status.h"

#include "memory_internal.h"
#include "memory_helpers.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdlib.h>
#include <string.h>

/* ── Working memory ────────────────────────────────────────────────────────── */

/**
 * @brief Create a working memory store with a capacity cap.
 *
 * @param[in]  out          Pointer to receive the new store.
 * @param[in]  max_capacity  Maximum items; 0 means unbounded.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
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
 * @brief Destroy a working memory store, freeing all items.
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
 * @brief Return the number of items in the working memory.
 */
size_t aegis_working_memory_count(const aegis_working_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

/**
 * @brief Insert or update a working memory item, evicting the lowest-priority
 * item if the capacity cap is exceeded.
 *
 * @param[in]  mem   Working memory store.
 * @param[in]  item  Item to insert (ownership transferred on success).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args or empty
 *         id/content, AEGIS_ERR_NOMEM on allocation failure.
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
 * @brief Retrieve the top-N highest-priority items.
 *
 * Returns a heap-allocated array of up to `n` items sorted by descending
 * priority. Items are not freed; caller owns the array.
 *
 * @param[in]  mem        Working memory store.
 * @param[in]  n          Maximum items to return.
 * @param[out] out        Receives the array of item pointers.
 * @param[out] out_count  Number of items returned.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *         AEGIS_ERR_NOMEM on allocation failure.
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
