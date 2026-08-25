#ifndef AEGIS_LIST_H
#define AEGIS_LIST_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file list.h
 * @brief Doubly-linked list with opaque handle.
 *
 * Thread safety: NOT thread-safe. Use a mutex to protect concurrent
 * access from multiple threads.
 *
 * Ownership: items are stored as raw pointers. The list does NOT
 * own, copy, or free items — the caller is fully responsible for
 * item lifetimes.
 */

/** Opaque list handle. */
typedef struct aegis_list aegis_list_t;

/**
 * @brief Create an empty list.
 *
 * @param[out] out  Receives the list handle. Ownership: transferred.
 * @return 0 on success, -1 on allocation failure or invalid @p out.
 */
int aegis_list_create(aegis_list_t** out);

/**
 * @brief Destroy the list and free all internal node memory.
 *
 * Does NOT free node payloads — the caller must clean up any
 * items that were pushed onto the list before destroying it.
 *
 * Safe to call with NULL (no-op).
 *
 * @param v Handle to destroy (ownership: consumed).
 */
void aegis_list_destroy(aegis_list_t* v);

/**
 * @brief Append an item to the back of the list.
 *
 * The item pointer is stored as-is (not copied). The caller must
 * ensure the item remains valid until it is removed from the list.
 *
 * @param v    List handle (borrowed).
 * @param item Item pointer to store (borrowed; owner retains responsibility).
 * @return 0 on success, -1 on allocation failure or invalid @p v.
 */
int aegis_list_push_back(aegis_list_t* v, const void* item);

/**
 * @brief Prepend an item to the front of the list.
 *
 * @param v    List handle (borrowed).
 * @param item Item pointer to store (borrowed).
 * @return 0 on success, -1 on allocation failure or invalid @p v.
 */
int aegis_list_push_front(aegis_list_t* v, const void* item);

/**
 * @brief Remove and return the item at the back of the list.
 *
 * @param v    List handle (borrowed).
 * @param[out] out  Receives the popped item pointer (may be NULL — no copy made).
 * @return 0 on success, -1 if the list is empty or invalid @p v.
 */
int aegis_list_pop_back(aegis_list_t* v, void* out);

/**
 * @brief Remove and return the item at the front of the list.
 *
 * @param v    List handle (borrowed).
 * @param[out] out  Receives the popped item pointer (may be NULL).
 * @return 0 on success, -1 if the list is empty or invalid @p v.
 */
int aegis_list_pop_front(aegis_list_t* v, void* out);

/**
 * @brief Peek at the front item without removing it.
 *
 * @param v    List handle (borrowed).
 * @param[out] out  Receives the front item pointer (may be NULL).
 * @return 0 on success, -1 if the list is empty or invalid @p v.
 */
int aegis_list_front(const aegis_list_t* v, const void** out);

/**
 * @brief Peek at the back item without removing it.
 *
 * @param v    List handle (borrowed).
 * @param[out] out  Receives the back item pointer (may be NULL).
 * @return 0 on success, -1 if the list is empty or invalid @p v.
 */
int aegis_list_back(const aegis_list_t* v, const void** out);

/**
 * @brief Return the number of items currently in the list.
 *
 * @param v List handle (borrowed; may be NULL → returns 0).
 * @return Item count.
 */
size_t aegis_list_len(const aegis_list_t* v);

/**
 * @brief Return true if the list contains no items.
 *
 * @param v List handle (borrowed; may be NULL → returns true).
 * @return true if empty.
 */
bool aegis_list_is_empty(const aegis_list_t* v);

/**
 * @brief Traverse the list, invoking @p fn on each item.
 *
 * Iteration order is front-to-back. If @p fn modifies the list
 * (e.g. removes the current node), behaviour is undefined.
 *
 * @param v   List handle (borrowed; may be NULL — no-op).
 * @param fn  Callback invoked for each item (must not be NULL).
 * @param ctx Opaque context passed to @p fn on every call.
 */
void aegis_list_for_each(aegis_list_t* v, void (*fn)(void* item, void* ctx), void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LIST_H */
