/**
 * @file dependency.c
 * @brief Dependency creation and accessors.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/task/dependency.h"
#include "dependency_internal.h"
#include <stdlib.h>

/**
 * @brief Allocate a dependency edge between two task IDs.
 *
 * @param[in] source  ID of the prerequisite (upstream) task.
 * @param[in] target  ID of the dependent (downstream) task.
 * @return New dependency, or NULL on allocation failure.
 *
 * @note Ownership: caller owns the result; release with
 *       aegis_dependency_destroy().
 */
aegis_dependency_t* aegis_dependency_create(uint32_t source, uint32_t target)
{
    aegis_dependency_t* dep = (aegis_dependency_t*)calloc(1, sizeof(*dep));
    if (!dep) {
        return NULL;
    }
    dep->source = source;
    dep->target = target;
    return dep;
}

/**
 * @brief Free a dependency created by aegis_dependency_create().
 *
 * @param[in] dep  Dependency to free; NULL is a no-op.
 */
void aegis_dependency_destroy(aegis_dependency_t* dep)
{
    free(dep);
}

/**
 * @brief Return the source (prerequisite task) ID of a dependency.
 *
 * @param[in] dep  Dependency to inspect; may be NULL.
 * @return Source task ID, or 0 when @p dep is NULL.
 */
uint32_t aegis_dependency_source(const aegis_dependency_t* dep)
{
    return dep ? dep->source : 0;
}

/**
 * @brief Return the target (dependent task) ID of a dependency.
 *
 * @param[in] dep  Dependency to inspect; may be NULL.
 * @return Target task ID, or 0 when @p dep is NULL.
 */
uint32_t aegis_dependency_target(const aegis_dependency_t* dep)
{
    return dep ? dep->target : 0;
}
