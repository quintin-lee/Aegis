/**
 * @file json_builder.c
 * @brief Provider-agnostic JSON body accumulator (shared LLM infra).
 *
 * A growable string buffer with OOM-checked growth, raw append and
 * escaped-JSON-string append. Used by each LLM provider to build request
 * bodies without repeating the realloc/escape logic.
 */
#include "llm_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aegis_json_builder_init(aegis_json_builder_t* b)
{
    if (b) {
        b->data = NULL;
        b->len  = 0;
        b->cap  = 0;
    }
}

void aegis_json_builder_free(aegis_json_builder_t* b)
{
    if (b) {
        free(b->data);
    }
}

/**
 * @brief Ensure capacity for @p extra more bytes plus the NUL terminator.
 *
 * Grows @p b in doubling steps. @return 1 on success, 0 on allocation
 * failure or overflow.
 */
static int json_builder_grow(aegis_json_builder_t* b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1) {
        return 0;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) {
        return 1;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }
    char* p = realloc(b->data, cap);
    if (!p) {
        return 0;
    }
    b->data = p;
    b->cap  = cap;
    return 1;
}

int aegis_json_append_raw(aegis_json_builder_t* b, const char* s, size_t n)
{
    if (!json_builder_grow(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

int aegis_json_append_string(aegis_json_builder_t* b, const char* s)
{
    if (!s) {
        s = "";
    }
    if (!aegis_json_append_raw(b, "\"", 1)) {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        char   esc[7];
        size_t n = 1;
        switch (*p) {
        case '"':
            esc[0] = '\\';
            esc[1] = '"';
            n      = 2;
            break;
        case '\\':
            esc[0] = '\\';
            esc[1] = '\\';
            n      = 2;
            break;
        case '\n':
            esc[0] = '\\';
            esc[1] = 'n';
            n      = 2;
            break;
        case '\r':
            esc[0] = '\\';
            esc[1] = 'r';
            n      = 2;
            break;
        case '\t':
            esc[0] = '\\';
            esc[1] = 't';
            n      = 2;
            break;
        default:
            if (*p < 0x20) {
                int written = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                if (written != 6) {
                    return 0;
                }
                n = 6;
            } else {
                esc[0] = (char)*p;
            }
            break;
        }
        if (!aegis_json_append_raw(b, esc, n)) {
            return 0;
        }
    }
    return aegis_json_append_raw(b, "\"", 1);
}
