#ifndef AEGIS_MEMORY_HELPERS_H
#define AEGIS_MEMORY_HELPERS_H

#include "aegis/memory/memory.h"

#include <stdlib.h>
#include <string.h>

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
