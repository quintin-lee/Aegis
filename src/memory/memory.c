
#define _POSIX_C_SOURCE 200809L
#include "aegis/memory/memory.h"
#include "aegis/status.h"

#include "memory_internal.h"
#include "memory_helpers.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdlib.h>
#include <string.h>

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

size_t aegis_memory_count(const aegis_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

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

void aegis_memory_item_destroy(aegis_memory_item_t* item)
{
    free_item(item);
}

/* ── Working memory ────────────────────────────────────────────────────────── */

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

size_t aegis_working_memory_count(const aegis_working_memory_t* mem)
{
    return mem ? aegis_vector_len(mem->items) : 0;
}

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
