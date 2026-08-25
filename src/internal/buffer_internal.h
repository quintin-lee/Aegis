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
