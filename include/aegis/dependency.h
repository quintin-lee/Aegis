/**
 * @file dependency.h
 * @brief Task dependency management API.
 *
 * A dependency represents a directed edge from one task to another:
 * "task B depends on task A" means A must complete before B can run.
 *
 * Dependencies are managed through the task graph. This header
 * provides the dependency type definition and factory functions.
 */
#ifndef AEGIS_DEPENDENCY_H
#define AEGIS_DEPENDENCY_H

#include "aegis/types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque dependency handle. */
typedef struct aegis_dependency aegis_dependency_t;

/**
 * @brief Create a dependency: target depends on source.
 *
 * @param source Task that must complete first (ID).
 * @param target Task that depends on source (ID).
 * @return Dependency handle (ownership: transferred), or NULL on failure.
 */
aegis_dependency_t* aegis_dependency_create(uint32_t source, uint32_t target);

/**
 * @brief Destroy a dependency.
 *
 * @param dep Handle to destroy (ownership: consumed).
 */
void aegis_dependency_destroy(aegis_dependency_t* dep);

/**
 * @brief Get the source task ID of a dependency.
 *
 * @param dep Dependency handle (borrowed).
 * @return Source task ID.
 */
uint32_t aegis_dependency_source(const aegis_dependency_t* dep);

/**
 * @brief Get the target task ID of a dependency.
 *
 * @param dep Dependency handle (borrowed).
 * @return Target task ID.
 */
uint32_t aegis_dependency_target(const aegis_dependency_t* dep);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_DEPENDENCY_H */
