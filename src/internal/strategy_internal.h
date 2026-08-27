/**
 * @file strategy_internal.h
 * @brief Internal layout of the strategy registry.
 *
 * Shared between strategy_registry.c and the planner binding. Not part
 * of any public ABI.
 */
#ifndef AEGIS_STRATEGY_INTERNAL_H
#define AEGIS_STRATEGY_INTERNAL_H

#include "aegis/strategy.h"

#include "aegis/common/mutex.h"
#include "aegis/common/hashmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** One registered strategy with owned storage. */
typedef struct aegis_strategy_entry {
    aegis_strategy_def_t def; /**< Shallow copy; strings stay borrowed. */
} aegis_strategy_entry_t;

struct aegis_strategy_registry {
    aegis_mutex_t*         lock;     /**< Leaf lock: never taken while held elsewhere. */
    aegis_hashmap_t*       map;      /**< name -> aegis_strategy_entry_t*. */
    aegis_strategy_entry_t** owned;  /**< Ownership side-array (hashmap frees values never). */
    size_t                 owned_len;
    size_t                 owned_cap;
};

/** Validate a registration input. AEGIS_OK or AEGIS_ERR_INVALID. */
aegis_status_t aegis_strategy_def_check(const aegis_strategy_def_t* def);

#endif /* AEGIS_STRATEGY_INTERNAL_H */
