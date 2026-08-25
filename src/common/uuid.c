/**
 * @file uuid.c
 * @brief UUID v4 generation, parsing and formatting.
 *
 * Generation uses /dev/urandom on POSIX; falls back to a pseudo-random
 * sequence seeded from wall-clock time if /dev/urandom is unavailable
 * (not cryptographically secure in that case).
 * Parsing accepts hyphenated (8-4-4-4-12) and raw hex (32-char) formats.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/uuid.h"
#include "aegis/common/time.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>

aegis_uuid_t aegis_uuid_null(void)
{
    aegis_uuid_t u;
    memset(u.bytes, 0, sizeof(u.bytes));
    return u;
}

aegis_uuid_t aegis_uuid_generate(void)
{
    aegis_uuid_t u  = aegis_uuid_null();
    int          fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, u.bytes, sizeof(u.bytes));
        close(fd);
        if (n == (ssize_t)sizeof(u.bytes)) {
            u.bytes[6] = (uint8_t)((u.bytes[6] & 0x0F) | 0x40);
            u.bytes[8] = (uint8_t)((u.bytes[8] & 0x3F) | 0x80);
            return u;
        }
    }
    uint64_t t = (uint64_t)aegis_wall_now();
    for (size_t i = 0; i < sizeof(u); i++) {
        u.bytes[i] = (uint8_t)((t >> (i * 8)) & 0xFF);
    }
    u.bytes[6] = (uint8_t)((u.bytes[6] & 0x0F) | 0x40);
    u.bytes[8] = (uint8_t)((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool aegis_uuid_parse(const char* str, aegis_uuid_t* out)
{
    if (!str || !out) {
        return false;
    }
    size_t hlen = strlen(str);

    if (hlen == 36) {
        if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
            return false;
        }
        out->bytes[0]  = (uint8_t)((hex_nibble(str[0]) << 4) | hex_nibble(str[1]));
        out->bytes[1]  = (uint8_t)((hex_nibble(str[2]) << 4) | hex_nibble(str[3]));
        out->bytes[2]  = (uint8_t)((hex_nibble(str[4]) << 4) | hex_nibble(str[5]));
        out->bytes[3]  = (uint8_t)((hex_nibble(str[6]) << 4) | hex_nibble(str[7]));
        out->bytes[4]  = (uint8_t)((hex_nibble(str[9]) << 4) | hex_nibble(str[10]));
        out->bytes[5]  = (uint8_t)((hex_nibble(str[11]) << 4) | hex_nibble(str[12]));
        out->bytes[6]  = (uint8_t)((hex_nibble(str[14]) << 4) | hex_nibble(str[15]));
        out->bytes[7]  = (uint8_t)((hex_nibble(str[16]) << 4) | hex_nibble(str[17]));
        out->bytes[8]  = (uint8_t)((hex_nibble(str[19]) << 4) | hex_nibble(str[20]));
        out->bytes[9]  = (uint8_t)((hex_nibble(str[21]) << 4) | hex_nibble(str[22]));
        out->bytes[10] = (uint8_t)((hex_nibble(str[24]) << 4) | hex_nibble(str[25]));
        out->bytes[11] = (uint8_t)((hex_nibble(str[26]) << 4) | hex_nibble(str[27]));
        out->bytes[12] = (uint8_t)((hex_nibble(str[28]) << 4) | hex_nibble(str[29]));
        out->bytes[13] = (uint8_t)((hex_nibble(str[30]) << 4) | hex_nibble(str[31]));
        out->bytes[14] = (uint8_t)((hex_nibble(str[32]) << 4) | hex_nibble(str[33]));
        out->bytes[15] = (uint8_t)((hex_nibble(str[34]) << 4) | hex_nibble(str[35]));
        return true;
    }

    if (hlen == 32) {
        for (size_t i = 0; i < 16; i++) {
            int hi = hex_nibble(str[i * 2]);
            int lo = hex_nibble(str[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out->bytes[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }

    return false;
}

void aegis_uuid_format(const aegis_uuid_t* u, char* buf, size_t buf_len)
{
    if (!u || !buf || buf_len < 37) {
        return;
    }
    const unsigned char* b = (const unsigned char*)u->bytes;
    snprintf(buf, buf_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}

bool aegis_uuid_eq(const aegis_uuid_t* a, const aegis_uuid_t* b)
{
    if (!a || !b) {
        return a == b;
    }
    return memcmp(a->bytes, b->bytes, sizeof(aegis_uuid_t)) == 0;
}

bool aegis_uuid_is_null(const aegis_uuid_t* u)
{
    if (!u) {
        return true;
    }
    aegis_uuid_t null_u = aegis_uuid_null();
    return memcmp(u->bytes, null_u.bytes, sizeof(aegis_uuid_t)) == 0;
}
