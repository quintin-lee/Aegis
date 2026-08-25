/**
 * @file buffer_internal.h
 * @brief Internal buffer struct layout for common module implementations.
 *
 * This header exposes the aegis_buffer struct definition so that
 * buffer.c and string.c can access fields directly. It must NOT
 * be included from public code — only src/common/ and related
 * internal sources may include it.
 */
#ifndef AEGIS_INTERNAL_BUFFER_H
#define AEGIS_INTERNAL_BUFFER_H

#include "aegis/common/buffer.h"

/* Internal layout — only for foundation module implementations. */
struct aegis_buffer {
    uint8_t* data;
    size_t   len;
    size_t   cap;
};

#endif /* AEGIS_INTERNAL_BUFFER_H */
