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
 * Thread safety: NOT thread-safe. Use a mutex to protect concurrent access.
 * Ownership: items are stored as raw pointers — the list does NOT own them.
 * Destroying the list does NOT free the items.
 */

typedef struct aegis_list aegis_list_t;

/**
 * @brief Create an empty list.
 *
 * @param[out] out Receives the list handle.
 * @return 0 on success, -1 on allocation failure or invalid out.
 */
int aegis_list_create(aegis_list_t** out);

/**
 * @brief Destroy the list and free all internal node memory.
 *
 * Does NOT free node payloads — caller is responsible for cleaning up items.
 */
void aegis_list_destroy(aegis_list_t* v);

/**
 * @brief Append an item to the back of the list.
 *
 * @param[in] v      List handle.
 * @param[in] item   Item pointer to store (copied by value).
 * @return 0 on success, -1 on allocation failure or invalid v.
 */
int aegis_list_push_back(aegis_list_t* v, const void* item);

/**
 * @brief Prepend an item to the front of the list.
 *
 * @param[in] v      List handle.
 * @param[in] item   Item pointer to store (copied by value).
 * @return 0 on success, -1 on allocation failure or invalid v.
 */
int aegis_list_push_front(aegis_list_t* v, const void* item);

/**
 * @brief Remove and return the item at the back of the list.
 *
 * @param[out] out   Receives the popped item. Ignored if NULL.
 * @return 0 on success, -1 if list is empty or invalid v.
 */
int aegis_list_pop_back(aegis_list_t* v, void* out);

/**
 * @brief Remove and return the item at the front of the list.
 *
 * @param[out] out   Receives the popped item. Ignored if NULL.
 * @return 0 on success, -1 if list is empty or invalid v.
 */
int aegis_list_pop_front(aegis_list_t* v, void* out);

/**
 * @brief Peek at the front item without removing it.
 *
 * @param[out] out   Receives the front item. Ignored if NULL.
 * @return 0 on success, -1 if list is empty or invalid v.
 */
int aegis_list_front(const aegis_list_t* v, const void** out);

/**
 * @brief Peek at the back item without removing it.
 *
 * @param[out] out   Receives the back item. Ignored if NULL.
 * @return 0 on success, -1 if list is empty or invalid v.
 */
int aegis_list_back(const aegis_list_t* v, const void** out);

/**
 * @brief Return the number of items currently in the list.
 */
size_t aegis_list_len(const aegis_list_t* v);

/**
 * @brief Return true if the list is empty.
 */
bool aegis_list_is_empty(const aegis_list_t* v);

/**
 * @brief Traverse the list, invoking fn on each item.
 *
 * @param[in] v      List handle.
 * @param[in] fn     Callback invoked for each item.
 * @param[in] ctx    Opaque context passed to fn.
 */
void aegis_list_for_each(aegis_list_t* v, void (*fn)(void* item, void* ctx), void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LIST_H */
