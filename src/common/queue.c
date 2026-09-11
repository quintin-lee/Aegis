/**
 * @file queue.c
 * @brief Fixed-capacity ring-buffer queue.
 *
 * Uses power-of-two capacity with bitmask wrapping for O(1) push/pop.
 * Items are stored as raw void* pointers; the queue does not own or
 * free them. aegis_queue_clear() zeroes slots but does not invoke
 * destructors on contained items.
 */
#include "aegis/common/queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/** Fixed-capacity ring buffer: power-of-two slots with bitmask wrapping. */
struct aegis_queue {
    void** slots;    /**< Slot array (borrowed item pointers). */
    size_t capacity; /**< Slot count, always a power of two. */
    size_t head;     /**< Pop index. */
    size_t tail;     /**< Push index. */
    size_t count;    /**< Occupied slots. */
};

/**
 * @brief Create a ring queue with exactly @p capacity slots.
 *
 * Capacity must be a non-zero power of two so head/tail wrap with a
 * bitmask. Items are stored borrowed — the queue never frees them.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param capacity Slot count (power of two, non-zero).
 * @return 0 on success, -1 on bad capacity, NULL out, or allocation failure.
 */
int aegis_queue_create(aegis_queue_t** out, size_t capacity)
{
    if (!out || capacity == 0 || (capacity & (capacity - 1)) != 0) {
        return -1;
    }
    aegis_queue_t* q = calloc(1, sizeof(*q));
    if (!q) {
        return -1;
    }
    q->slots = (void**)calloc(capacity, sizeof(void*));
    if (!q->slots) {
        free(q);
        return -1;
    }
    q->capacity = capacity;
    *out        = q;
    return 0;
}

/**
 * @brief Destroy a queue and its slot array (items are borrowed, not freed).
 *
 * Safe to call with NULL (no-op).
 *
 * @param q Queue to destroy (ownership: consumed).
 */
void aegis_queue_destroy(aegis_queue_t* q)
{
    if (!q) {
        return;
    }
    free(q->slots);
    free(q);
}

/**
 * @brief Push an item at the tail (stored borrowed).
 *
 * Rejected when the queue is full — capacity is fixed, never grown.
 *
 * @param q    Queue (borrowed).
 * @param item Item pointer to store (borrowed; caller keeps ownership).
 * @return 0 on success, -1 on NULL queue or when full.
 */
int aegis_queue_push(aegis_queue_t* q, void* item)
{
    if (!q || aegis_queue_is_full(q)) {
        return -1;
    }
    q->slots[q->tail] = item;
    q->tail           = (q->tail + 1) & (q->capacity - 1);
    q->count++;
    return 0;
}

/**
 * @brief Pop the head item; the freed slot is cleared to NULL.
 *
 * @param q   Queue (borrowed).
 * @param out Optional destination for the borrowed item pointer (may be NULL).
 * @return 0 on success, -1 on NULL queue or when empty.
 */
int aegis_queue_pop(aegis_queue_t* q, void* out)
{
    if (!q || aegis_queue_is_empty(q)) {
        return -1;
    }
    if (out) {
        *(void**)out = q->slots[q->head];
    }
    q->slots[q->head] = NULL;
    q->head           = (q->head + 1) & (q->capacity - 1);
    q->count--;
    return 0;
}

/**
 * @brief Peek at the head item without removing it.
 *
 * @param q   Queue (borrowed).
 * @param out Optional destination for the borrowed item pointer (may be NULL).
 * @return 0 on success, -1 on NULL queue or when empty.
 */
int aegis_queue_peek(const aegis_queue_t* q, void* out)
{
    if (!q || aegis_queue_is_empty(q)) {
        return -1;
    }
    if (out) {
        *(void**)out = q->slots[q->head];
    }
    return 0;
}

/**
 * @brief Return the occupied-slot count (0 for NULL input).
 *
 * @param q Queue (borrowed).
 * @return Occupied slots.
 */
size_t aegis_queue_len(const aegis_queue_t* q)
{
    return q ? q->count : 0;
}

/**
 * @brief Test whether the queue holds no items (NULL counts as empty).
 *
 * @param q Queue (borrowed).
 * @return true when empty or NULL, false otherwise.
 */
bool aegis_queue_is_empty(const aegis_queue_t* q)
{
    return !q || q->count == 0;
}

/**
 * @brief Test whether all slots are occupied (NULL counts as not full).
 *
 * @param q Queue (borrowed).
 * @return true when count >= capacity, false otherwise.
 */
bool aegis_queue_is_full(const aegis_queue_t* q)
{
    return q && q->count >= q->capacity;
}

/**
 * @brief Drop all items and reset head/tail/count, keeping the allocation.
 *
 * Slots are zeroed but contained items are borrowed and never destructed.
 * No-op for NULL input.
 *
 * @param q Queue to clear (borrowed).
 */
void aegis_queue_clear(aegis_queue_t* q)
{
    if (!q) {
        return;
    }
    memset(q->slots, 0, q->capacity * sizeof(void*));
    q->head = q->tail = q->count = 0;
}
