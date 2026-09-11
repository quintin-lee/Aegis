/**
 * @file list.c
 * @brief Doubly-linked list implementation.
 *
 * Nodes are heap-allocated; items are stored as raw pointers without
 * copying. The list does not own or free items — the caller is
 * responsible for item lifetimes. aegis_list_destroy() frees only
 * internal node structures.
 */
#include "aegis/common/list.h"
#include <stdlib.h>

/** Doubly-linked node holding one borrowed item pointer. */
struct aegis_list_node {
    void*                   item; /**< Borrowed item (caller-owned). */
    struct aegis_list_node* prev; /**< Previous node, NULL at head. */
    struct aegis_list_node* next; /**< Next node, NULL at tail. */
};

/** List head/tail anchors plus cached length. */
struct aegis_list {
    struct aegis_list_node* head; /**< First node, NULL when empty. */
    struct aegis_list_node* tail; /**< Last node, NULL when empty. */
    size_t                  len;  /**< Node count. */
};

/**
 * @brief Create an empty list.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @return 0 on success, -1 on NULL out or allocation failure.
 */
int aegis_list_create(aegis_list_t** out)
{
    if (!out) {
        return -1;
    }
    aegis_list_t* v = calloc(1, sizeof(*v));
    if (!v) {
        return -1;
    }
    *out = v;
    return 0;
}

/**
 * @brief Destroy a list and its nodes (items are borrowed, not freed).
 *
 * Safe to call with NULL (no-op).
 *
 * @param v List to destroy (ownership: consumed).
 */
void aegis_list_destroy(aegis_list_t* v)
{
    if (!v) {
        return;
    }
    struct aegis_list_node* node = v->head;
    while (node) {
        struct aegis_list_node* next = node->next;
        free(node);
        node = next;
    }
    free(v);
}

/**
 * @brief Append an item pointer at the tail (stored borrowed, not copied).
 *
 * @param v    List (borrowed).
 * @param item Item pointer to store (borrowed; caller keeps ownership).
 * @return 0 on success, -1 on NULL list or allocation failure.
 */
int aegis_list_push_back(aegis_list_t* v, const void* item)
{
    if (!v) {
        return -1;
    }
    struct aegis_list_node* node = malloc(sizeof(*node));
    if (!node) {
        return -1;
    }
    node->item = (void*)item;
    node->prev = v->tail;
    node->next = NULL;
    if (v->tail) {
        v->tail->next = node;
    } else {
        v->head = node;
    }
    v->tail = node;
    v->len++;
    return 0;
}

/**
 * @brief Prepend an item pointer at the head (stored borrowed, not copied).
 *
 * @param v    List (borrowed).
 * @param item Item pointer to store (borrowed; caller keeps ownership).
 * @return 0 on success, -1 on NULL list or allocation failure.
 */
int aegis_list_push_front(aegis_list_t* v, const void* item)
{
    if (!v) {
        return -1;
    }
    struct aegis_list_node* node = malloc(sizeof(*node));
    if (!node) {
        return -1;
    }
    node->item = (void*)item;
    node->prev = NULL;
    node->next = v->head;
    if (v->head) {
        v->head->prev = node;
    } else {
        v->tail = node;
    }
    v->head = node;
    v->len++;
    return 0;
}

/**
 * @brief Remove the tail node and optionally return its item.
 *
 * Only the node is freed; the item itself stays caller-owned.
 *
 * @param v   List (borrowed).
 * @param out Optional destination for the borrowed item pointer (may be NULL).
 * @return 0 on success, -1 when the list is NULL or empty.
 */
int aegis_list_pop_back(aegis_list_t* v, void* out)
{
    if (!v || !v->tail) {
        return -1;
    }
    struct aegis_list_node* node = v->tail;
    if (out) {
        *(void**)out = node->item;
    }
    v->tail = node->prev;
    if (v->tail) {
        v->tail->next = NULL;
    } else {
        v->head = NULL;
    }
    free(node);
    v->len--;
    return 0;
}

/**
 * @brief Remove the head node and optionally return its item.
 *
 * Only the node is freed; the item itself stays caller-owned.
 *
 * @param v   List (borrowed).
 * @param out Optional destination for the borrowed item pointer (may be NULL).
 * @return 0 on success, -1 when the list is NULL or empty.
 */
int aegis_list_pop_front(aegis_list_t* v, void* out)
{
    if (!v || !v->head) {
        return -1;
    }
    struct aegis_list_node* node = v->head;
    if (out) {
        *(void**)out = node->item;
    }
    v->head = node->next;
    if (v->head) {
        v->head->prev = NULL;
    } else {
        v->tail = NULL;
    }
    free(node);
    v->len--;
    return 0;
}

/**
 * @brief Peek at the head item without removing it.
 *
 * @param v   List (borrowed).
 * @param[out] out Receives the borrowed item pointer (may be NULL to skip).
 * @return 0 on success, -1 when the list is NULL or empty.
 */
int aegis_list_front(const aegis_list_t* v, const void** out)
{
    if (!v || !v->head) {
        return -1;
    }
    if (out) {
        *out = v->head->item;
    }
    return 0;
}

/**
 * @brief Peek at the tail item without removing it.
 *
 * @param v   List (borrowed).
 * @param[out] out Receives the borrowed item pointer (may be NULL to skip).
 * @return 0 on success, -1 when the list is NULL or empty.
 */
int aegis_list_back(const aegis_list_t* v, const void** out)
{
    if (!v || !v->tail) {
        return -1;
    }
    if (out) {
        *out = v->tail->item;
    }
    return 0;
}

/**
 * @brief Return the node count (0 for NULL input).
 *
 * @param v List (borrowed).
 * @return Node count.
 */
size_t aegis_list_len(const aegis_list_t* v)
{
    return v ? v->len : 0;
}

/**
 * @brief Test whether the list holds no nodes (NULL counts as empty).
 *
 * @param v List (borrowed).
 * @return true when empty or NULL, false otherwise.
 */
bool aegis_list_is_empty(const aegis_list_t* v)
{
    return !v || v->len == 0;
}

/**
 * @brief Invoke @p fn on every item from head to tail.
 *
 * No-op when the list or callback is NULL. The callback must not mutate
 * the list structure itself.
 *
 * @param v   List (borrowed).
 * @param fn  Visitor called per borrowed item (borrowed).
 * @param ctx Opaque argument forwarded to @p fn (borrowed).
 */
void aegis_list_for_each(aegis_list_t* v, void (*fn)(void* item, void* ctx), void* ctx)
{
    if (!v || !fn) {
        return;
    }
    for (struct aegis_list_node* node = v->head; node; node = node->next) {
        fn(node->item, ctx);
    }
}
