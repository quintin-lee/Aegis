#ifndef AEGIS_INTERNAL_LIFECYCLE_H
#define AEGIS_INTERNAL_LIFECYCLE_H

#include "aegis/types.h"

/* Internal helpers — do NOT include this header from public code. */

/**
 * @brief Check that an output pointer is valid before writing.
 */
#define AEGIS_CHECK_OUT(out) \
    do { \
        if (!(out)) { return AEGIS_ERR_INVALID; } \
    } while (0)

/**
 * @brief Guard against double-free by NULLing after release.
 */
#define AEGIS_SAFE_FREE(ptr)      \
    do {                          \
        free(ptr);                \
        (ptr) = NULL;             \
    } while (0)

#endif /* AEGIS_INTERNAL_LIFECYCLE_H */
