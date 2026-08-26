/**
 * @file dependency_internal.h
 * @brief Internal dependency struct layout.
 *
 * NOT part of the public API.
 */
#ifndef AEGIS_DEPENDENCY_INTERNAL_H
#define AEGIS_DEPENDENCY_INTERNAL_H

#include "aegis/dependency.h"

/** Internal dependency structure. */
struct aegis_dependency {
    uint32_t source;
    uint32_t target;
};

#endif /* AEGIS_DEPENDENCY_INTERNAL_H */
