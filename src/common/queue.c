#include "aegis/common/queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

struct aegis_queue {
    void** slots;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
};

int aegis_queue_create(aegis_queue_t** out, size_t capacity)
{
    if (!out || capacity == 0 || (capacity & (capacity - 1)) != 0)
        return -1;
    aegis_queue_t* q = calloc(1, sizeof(*q));
    if (!q)
        return -1;
    q->slots = (void**)calloc(capacity, sizeof(void*));
    if (!q->slots) {
        free(q);
        return -1;
    }
    q->capacity = capacity;
    *out        = q;
    return 0;
}

void aegis_queue_destroy(aegis_queue_t* q)
{
    if (!q)
        return;
    free(q->slots);
    free(q);
}

int aegis_queue_push(aegis_queue_t* q, void* item)
{
    if (!q || aegis_queue_is_full(q))
        return -1;
    q->slots[q->tail] = item;
    q->tail           = (q->tail + 1) & (q->capacity - 1);
    q->count++;
    return 0;
}

int aegis_queue_pop(aegis_queue_t* q, void* out)
{
    if (!q || aegis_queue_is_empty(q))
        return -1;
    if (out)
        *(void**)out = q->slots[q->head];
    q->slots[q->head] = NULL;
    q->head           = (q->head + 1) & (q->capacity - 1);
    q->count--;
    return 0;
}

int aegis_queue_peek(const aegis_queue_t* q, void* out)
{
    if (!q || aegis_queue_is_empty(q))
        return -1;
    if (out)
        *(void**)out = q->slots[q->head];
    return 0;
}

size_t aegis_queue_len(const aegis_queue_t* q)
{
    return q ? q->count : 0;
}

bool aegis_queue_is_empty(const aegis_queue_t* q)
{
    return !q || q->count == 0;
}

bool aegis_queue_is_full(const aegis_queue_t* q)
{
    return q && q->count >= q->capacity;
}

void aegis_queue_clear(aegis_queue_t* q)
{
    if (!q)
        return;
    memset(q->slots, 0, q->capacity * sizeof(void*));
    q->head = q->tail = q->count = 0;
}
