#ifndef AEGIS_QUEUE_H
#define AEGIS_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file queue.h
 * @brief Fixed-capacity ring-buffer queue.
 *
 * Thread safety: NOT thread-safe. Use a mutex to protect concurrent
 * access from multiple threads.
 *
 * Capacity must be a power of two for efficient index wrapping
 * (bitmask instead of modulo division).
 */

/** Opaque queue handle. */
typedef struct aegis_queue aegis_queue_t;

/**
 * @brief Create a queue with the given capacity.
 *
 * @param[out] out       Receives the queue handle. Ownership: transferred.
 * @param[in]  capacity  Number of slots (must be a power of two, >= 1).
 * @return 0 on success, -1 if @p capacity is not a power of two or @p out is NULL.
 */
int aegis_queue_create(aegis_queue_t** out, size_t capacity);

/**
 * @brief Destroy a queue and free all backing storage.
 *
 * Does NOT free items that were pushed onto the queue — the caller
 * is responsible for cleaning those up separately.
 *
 * Safe to call with NULL (no-op).
 *
 * @param q Handle to destroy (ownership: consumed).
 */
void aegis_queue_destroy(aegis_queue_t* q);

/**
 * @brief Push an item onto the back of the queue.
 *
 * The item pointer is stored as-is. The caller retains ownership
 * and is responsible for ensuring the item remains valid until
 * it is popped.
 *
 * @param q    Queue handle (borrowed).
 * @param item Item pointer to enqueue (borrowed).
 * @return 0 on success, -1 if the queue is full or @p q is NULL.
 */
int aegis_queue_push(aegis_queue_t* q, void* item);

/**
 * @brief Pop an item from the front of the queue.
 *
 * @param[in]  q    Queue handle (borrowed).
 * @param[out] out  Receives the popped item pointer (may be NULL — no copy made).
 * @return 0 on success, -1 if the queue is empty or @p q is NULL.
 */
int aegis_queue_pop(aegis_queue_t* q, void* out);

/**
 * @brief Peek at the front item without removing it.
 *
 * @param[in]  q    Queue handle (borrowed).
 * @param[out] out  Receives a pointer to the front item (may be NULL).
 * @return 0 on success, -1 if the queue is empty or @p q is NULL.
 */
int aegis_queue_peek(const aegis_queue_t* q, void* out);

/**
 * @brief Return the number of items currently in the queue.
 *
 * @param q Queue handle (borrowed; may be NULL → returns 0).
 * @return Item count.
 */
size_t aegis_queue_len(const aegis_queue_t* q);

/**
 * @brief Return true if the queue contains no items.
 *
 * @param q Queue handle (borrowed; may be NULL → returns true).
 * @return true if empty.
 */
bool aegis_queue_is_empty(const aegis_queue_t* q);

/**
 * @brief Return true if the queue has reached its capacity.
 *
 * @param q Queue handle (borrowed; may be NULL → returns true).
 * @return true if full.
 */
bool aegis_queue_is_full(const aegis_queue_t* q);

/**
 * @brief Clear all items from the queue.
 *
 * Does NOT invoke destructors or free items — the caller is
 * responsible for cleaning up any items that were in the queue.
 *
 * @param q Queue handle (borrowed; may be NULL — no-op).
 */
void aegis_queue_clear(aegis_queue_t* q);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_QUEUE_H */
