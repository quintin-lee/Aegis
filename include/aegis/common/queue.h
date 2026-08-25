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
 * @brief Fixed-capacity ring buffer queue.
 *
 * Thread safety: NOT thread-safe. Use a mutex to protect concurrent access.
 * Capacity must be a power of two for efficient modulo.
 */

typedef struct aegis_queue aegis_queue_t;

/**
 * @brief Create a queue with the given capacity (must be power of two, >= 1).
 *
 * @param[out] out      Receives the queue handle.
 * @param[in]  capacity Number of slots.
 * @return AEGIS_ERR_INVALID if capacity is not a power of two.
 */
int aegis_queue_create(aegis_queue_t** out, size_t capacity);

/**
 * @brief Destroy a queue and free all resources.
 */
void aegis_queue_destroy(aegis_queue_t* q);

/**
 * @brief Push an item onto the back of the queue.
 *
 * @return 0 on success, -1 if queue is full.
 */
int aegis_queue_push(aegis_queue_t* q, void* item);

/**
 * @brief Pop an item from the front of the queue.
 *
 * @param[out] out      Receives the popped item. Ignored if NULL.
 * @return 0 on success, -1 if queue is empty.
 */
int aegis_queue_pop(aegis_queue_t* q, void* out);

/**
 * @brief Peek at the front item without removing it.
 *
 * @return 0 on success, -1 if empty.
 */
int aegis_queue_peek(const aegis_queue_t* q, void* out);

/**
 * @brief Return the number of items currently in the queue.
 */
size_t aegis_queue_len(const aegis_queue_t* q);

/**
 * @brief Return true if the queue is empty.
 */
bool aegis_queue_is_empty(const aegis_queue_t* q);

/**
 * @brief Return true if the queue is full.
 */
bool aegis_queue_is_full(const aegis_queue_t* q);

/**
 * @brief Clear all items from the queue.
 *
 * Does NOT invoke destructors — caller is responsible for cleaning up items.
 */
void aegis_queue_clear(aegis_queue_t* q);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_QUEUE_H */
