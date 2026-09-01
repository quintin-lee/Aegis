#ifndef AEGIS_CODING_MUTATIONS_H
#define AEGIS_CODING_MUTATIONS_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mutations.h
 * @brief Per-path file mutation serialization.
 */

typedef struct aegis_mutation_queue aegis_mutation_queue_t;

aegis_status_t aegis_mutation_queue_create(aegis_mutation_queue_t** out);
void           aegis_mutation_queue_destroy(aegis_mutation_queue_t* q);

// Acquire exclusive lock for path (blocks until previous mutation for same path completes)
aegis_status_t aegis_mutation_queue_acquire(aegis_mutation_queue_t* q, const char* path);
void           aegis_mutation_queue_release(aegis_mutation_queue_t* q, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_MUTATIONS_H */
