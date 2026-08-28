/**
 * @file tool_internal.h
 * @brief Internal layouts for the tool module.
 *
 * NOT part of the public API.
 */
#ifndef AEGIS_TOOL_INTERNAL_H
#define AEGIS_TOOL_INTERNAL_H

#include "aegis/common/hashmap.h"
#include "aegis/common/mutex.h"
#include "aegis/tool/tool.h"

/** One owned name -> owned-value binding inside an argument list. */
typedef struct aegis_tool_arg_entry {
    char*              name;  /**< malloc'd copy, NUL-terminated. */
    aegis_tool_value_t value; /**< Payload heap-owned for STRING/BYTES. */
} aegis_tool_arg_entry_t;

struct aegis_tool_args {
    aegis_tool_arg_entry_t* items; /**< Owned dynamic array. */
    size_t                  len;
    size_t                  cap;
};

struct aegis_tool_registry {
    aegis_mutex_t*   lock; /**< Guards map and owned array. Order: registry lock only (leaf). */
    aegis_hashmap_t* map;  /**< name -> aegis_tool_def_t (shallow copies). */
    /**
     * Owned copies of every stored def. The hashmap does not free values
     * and exposes no iteration, so this parallel array is the ownership
     * record used by destroy. Pointers are address-stable for the
     * registry lifetime.
     */
    aegis_tool_def_t** owned;
    size_t             owned_len;
    size_t             owned_cap;
};

/** Bridge descriptor binding one pending task to one tool invocation. */
struct aegis_tool_job {
    aegis_tool_registry_t* reg;  /**< Borrowed. */
    char*                  name; /**< Owned copy of the tool name. */
    aegis_tool_args_t*     args; /**< TRANSFERRED from aegis_tool_job_create. */
};

/** Release one entry's name and payload (not the entry slot itself). */
void aegis_tool_arg_entry_clear(aegis_tool_arg_entry_t* entry);

#endif /* AEGIS_TOOL_INTERNAL_H */
