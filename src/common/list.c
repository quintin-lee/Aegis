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

struct aegis_list_node {
    void*                   item;
    struct aegis_list_node* prev;
    struct aegis_list_node* next;
};

struct aegis_list {
    struct aegis_list_node* head;
    struct aegis_list_node* tail;
    size_t                  len;
};

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

size_t aegis_list_len(const aegis_list_t* v)
{
    return v ? v->len : 0;
}

bool aegis_list_is_empty(const aegis_list_t* v)
{
    return !v || v->len == 0;
}

void aegis_list_for_each(aegis_list_t* v, void (*fn)(void* item, void* ctx), void* ctx)
{
    if (!v || !fn) {
        return;
    }
    for (struct aegis_list_node* node = v->head; node; node = node->next) {
        fn(node->item, ctx);
    }
}
