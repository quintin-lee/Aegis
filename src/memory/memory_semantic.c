/* ── Semantic memory ───────────────────────────────────────────────────────── */
#define _POSIX_C_SOURCE 200809L
#include "aegis/memory/memory.h"
#include "aegis/status.h"

#include "memory_internal.h"
#include "memory_helpers.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdlib.h>
#include <string.h>

/* ── Semantic memory ───────────────────────────────────────────────────────── */

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

size_t aegis_semantic_memory_count(const aegis_semantic_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

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
