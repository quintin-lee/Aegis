/**
 * @file dependency.c
 * @brief Dependency creation and accessors.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/dependency.h"
#include "internal/dependency_internal.h"
#include <stdlib.h>

aegis_dependency_t* aegis_dependency_create(uint32_t source, uint32_t target) {
    aegis_dependency_t* dep = (aegis_dependency_t*)calloc(1, sizeof(*dep));
    if (!dep) return NULL;
    dep->source = source;
    dep->target = target;
    return dep;
}

void aegis_dependency_destroy(aegis_dependency_t* dep) {
    free(dep);
}

uint32_t aegis_dependency_source(const aegis_dependency_t* dep) {
    return dep ? dep->source : 0;
}

uint32_t aegis_dependency_target(const aegis_dependency_t* dep) {
    return dep ? dep->target : 0;
}
