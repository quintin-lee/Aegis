/**
 * @file memory_internal.h
 * @brief Internal layouts for memory module data structures.
 *
 * NOT part of the public ABI.
 */
#ifndef AEGIS_MEMORY_INTERNAL_H
#define AEGIS_MEMORY_INTERNAL_H

#include "aegis/memory.h"
#include "aegis/common/vector.h"

#include <stdint.h>
#include <stddef.h>

/* ── Generic memory store ─────────────────────────────────────────────────── */

struct aegis_memory {
    aegis_vector_t* items; /**< Vector of aegis_memory_item_t*. */
};

/* ── Working memory ────────────────────────────────────────────────────────── */

struct aegis_working_memory {
    aegis_vector_t* items;       /**< Vector of aegis_memory_item_t*. */
    size_t          max_capacity; /**< 0 = unlimited. */
};

/* ── Episodic memory ───────────────────────────────────────────────────────── */

struct aegis_episodic_memory {
    aegis_vector_t* items; /**< Vector of aegis_memory_item_t*, ordered by timestamp. */
};

/* ── Semantic memory ───────────────────────────────────────────────────────── */

struct aegis_semantic_memory {
    aegis_vector_t* items; /**< Vector of aegis_memory_item_t*. */
};

/* ── Procedural memory ─────────────────────────────────────────────────────── */

struct aegis_procedural_memory {
    aegis_vector_t* items; /**< Vector of aegis_memory_item_t*. */
};

#endif /* AEGIS_MEMORY_INTERNAL_H */
