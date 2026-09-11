/**
 * @file memory_helpers.h
 * @brief Shared inline helpers for the memory subsystem (NOT public API).
 *
 * Provides deep-clone, destruction, and priority ordering for
 * aegis_memory_item_t. All helpers are static inline so every .c file
 * including this header gets its own copy.
 */
#ifndef AEGIS_MEMORY_HELPERS_H
#define AEGIS_MEMORY_HELPERS_H

#include "aegis/memory/memory.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Deep-clone a memory item including id, content, and metadata.
 *
 * Every string is duplicated, so the clone is fully independent of @p src.
 * On any allocation failure all partial state is released and NULL returned.
 *
 * @param src Item to clone (borrowed; NULL yields NULL).
 * @return Fresh clone (ownership: transferred), or NULL on NULL input / failure.
 */
static inline aegis_memory_item_t* clone_item(const aegis_memory_item_t* src)
{
    if (!src) {
        return NULL;
    }
    aegis_memory_item_t* dst = calloc(1, sizeof(*dst));
    if (!dst) {
        return NULL;
    }
    dst->id        = src->id ? strdup(src->id) : NULL;
    dst->content   = src->content ? strdup(src->content) : NULL;
    dst->type      = src->type;
    dst->timestamp = src->timestamp;
    dst->priority  = src->priority;
    if (src->metadata_keys && src->metadata_vals && src->n_metadata > 0) {
        dst->n_metadata    = src->n_metadata;
        dst->metadata_keys = malloc(sizeof(char*) * src->n_metadata);
        dst->metadata_vals = malloc(sizeof(char*) * src->n_metadata);
        if (!dst->metadata_keys || !dst->metadata_vals) {
            free(dst->metadata_keys);
            free(dst->metadata_vals);
            dst->metadata_keys = NULL;
            dst->metadata_vals = NULL;
            dst->n_metadata    = 0;
            free(dst->id);
            free(dst->content);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < src->n_metadata; i++) {
            dst->metadata_keys[i] = src->metadata_keys[i] ? strdup(src->metadata_keys[i]) : NULL;
            dst->metadata_vals[i] = src->metadata_vals[i] ? strdup(src->metadata_vals[i]) : NULL;
        }
    }
    return dst;
}

/**
 * @brief Destroy an item and every string it owns. Safe for NULL (no-op).
 *
 * Frees id, content, each metadata key/value pair, the key/value arrays,
 * and the item itself.
 *
 * @param item Item to destroy (ownership: consumed).
 */
static inline void free_item(aegis_memory_item_t* item)
{
    if (!item) {
        return;
    }
    free(item->id);
    free(item->content);
    if (item->metadata_keys) {
        for (size_t i = 0; i < item->n_metadata; i++) {
            free((void*)item->metadata_keys[i]);
            free((void*)item->metadata_vals[i]);
        }
        free(item->metadata_keys);
        free(item->metadata_vals);
    }
    free(item);
}

/**
 * @brief qsort comparator: priority descending, then timestamp descending.
 *
 * NULL entries sort as equal (return 0) to keep the comparator total
 * without dereferencing nulls.
 *
 * @param a Pointer to the left item pointer.
 * @param b Pointer to the right item pointer.
 * @return Negative when @p a sorts first, positive when @p b does, 0 when tied.
 */
static inline int cmp_item_by_priority_desc(const void* a, const void* b)
{
    const aegis_memory_item_t* const* x = (const aegis_memory_item_t* const*)a;
    const aegis_memory_item_t* const* y = (const aegis_memory_item_t* const*)b;
    if (!*x || !*y) {
        return 0;
    }
    if ((*x)->priority > (*y)->priority) {
        return -1;
    }
    if ((*x)->priority < (*y)->priority) {
        return 1;
    }
    if ((*x)->timestamp > (*y)->timestamp) {
        return -1;
    }
    if ((*x)->timestamp < (*y)->timestamp) {
        return 1;
    }
    return 0;
}

#endif /* AEGIS_MEMORY_HELPERS_H */
