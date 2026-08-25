#include "aegis/common/buffer.h"
#include <stdlib.h>
#include <string.h>

struct aegis_buffer {
    uint8_t* data;
    size_t   len;
    size_t   cap;
};

#define BUFFER_INIT_CAP 64

int aegis_buffer_create(aegis_buffer_t** out, size_t init_cap)
{
    if (!out) {
        return -1;
    }
    aegis_buffer_t* buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return -1;
    }
    size_t cap = init_cap ? init_cap : BUFFER_INIT_CAP;
    buf->data  = (uint8_t*)calloc(cap, 1);
    if (!buf->data) {
        free(buf);
        return -1;
    }
    buf->cap = cap;
    *out     = buf;
    return 0;
}

void aegis_buffer_destroy(aegis_buffer_t* buf)
{
    if (!buf) {
        return;
    }
    free(buf->data);
    free(buf);
}

static int buffer_grow(aegis_buffer_t* buf, size_t need)
{
    size_t new_cap = buf->cap;
    while (new_cap < buf->len + need) {
        new_cap *= 2;
    }
    uint8_t* next = (uint8_t*)realloc(buf->data, new_cap);
    if (!next) {
        return -1;
    }
    buf->data = next;
    buf->cap  = new_cap;
    return 0;
}

int aegis_buffer_append(aegis_buffer_t* buf, const uint8_t* data, size_t len)
{
    if (!buf || !data) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (buffer_grow(buf, len) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

int aegis_buffer_append_byte(aegis_buffer_t* buf, uint8_t byte)
{
    return aegis_buffer_append(buf, &byte, 1);
}

int aegis_buffer_append_str(aegis_buffer_t* buf, const char* str)
{
    if (!buf || !str) {
        return -1;
    }
    size_t len = strlen(str);
    return aegis_buffer_append(buf, (const uint8_t*)str, len);
}

int aegis_buffer_read(const aegis_buffer_t* buf, uint8_t* out, size_t offset, size_t len)
{
    if (!buf || !out || offset + len > buf->len) {
        return -1;
    }
    memcpy(out, buf->data + offset, len);
    return 0;
}

const uint8_t* aegis_buffer_data(const aegis_buffer_t* buf)
{
    return buf ? buf->data : NULL;
}

size_t aegis_buffer_len(const aegis_buffer_t* buf)
{
    return buf ? buf->len : 0;
}

size_t aegis_buffer_capacity(const aegis_buffer_t* buf)
{
    return buf ? buf->cap : 0;
}

void aegis_buffer_clear(aegis_buffer_t* buf)
{
    if (buf) {
        buf->len = 0;
    }
}

int aegis_buffer_reserve(aegis_buffer_t* buf, size_t min_cap)
{
    if (!buf) {
        return -1;
    }
    if (buf->cap >= min_cap) {
        return 0;
    }
    uint8_t* next = (uint8_t*)realloc(buf->data, min_cap);
    if (!next) {
        return -1;
    }
    buf->data = next;
    buf->cap  = min_cap;
    return 0;
}
