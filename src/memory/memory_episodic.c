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

size_t aegis_episodic_memory_count(const aegis_episodic_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

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


