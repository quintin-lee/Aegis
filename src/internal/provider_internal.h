#ifndef AEGIS_PROVIDER_INTERNAL_H
#define AEGIS_PROVIDER_INTERNAL_H

#include "aegis/common/hashmap.h"
#include "aegis/common/mutex.h"
#include "aegis/provider.h"

#include <stddef.h>

/**
 * @file provider_internal.h
 * @brief Internal layout of the provider registry. Not part of any ABI.
 */

/** Registry entry: owned definition copy plus mutable lifecycle state. */
typedef struct aegis_provider_entry {
    aegis_provider_def_t   def;   /**< Shallow copy; strings remain borrowed.  */
    aegis_provider_state_t state; /**< Guarded by registry lock.               */
} aegis_provider_entry_t;

/** Provider registry: mutex-guarded name map plus ownership side-array. */
struct aegis_provider_registry {
    aegis_mutex_t*            lock;      /**< Leaf lock for all map/state access. */
    aegis_hashmap_t*          map;       /**< name -> aegis_provider_entry_t*.    */
    aegis_provider_entry_t**  owned;     /**< Ownership side-array (map frees nothing). */
    size_t                    owned_len;
    size_t                    owned_cap;
};

/**
 * @brief Validate a registration input.
 *
 * Checks def non-NULL, name non-NULL and non-empty, and
 * abi_version == AEGIS_PROVIDER_ABI_VERSION.
 *
 * @return AEGIS_OK or AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_provider_def_check(const aegis_provider_def_t* def);

/**
 * @brief Copy the entry's def into @p view and snapshot its state.
 * Caller must hold reg->lock (or have exclusive access).
 */
void aegis_provider_entry_view(const aegis_provider_entry_t* entry, aegis_provider_view_t* view);

#endif /* AEGIS_PROVIDER_INTERNAL_H */
