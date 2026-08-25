#include "aegis/common/buffer.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    aegis_buffer_t* buf = NULL;
    assert(aegis_buffer_create(&buf, 0) == 0);
    assert(aegis_buffer_len(buf) == 0);
    assert(aegis_buffer_capacity(buf) > 0);

    /* Append bytes */
    uint8_t data[] = {0x01, 0x02, 0x03};
    assert(aegis_buffer_append(buf, data, 3) == 0);
    assert(aegis_buffer_len(buf) == 3);

    /* Append string */
    assert(aegis_buffer_append_str(buf, "hello") == 0);
    assert(aegis_buffer_len(buf) == 8);

    /* Read back */
    uint8_t out[16] = {0};
    assert(aegis_buffer_read(buf, out, 0, 3) == 0);
    assert(out[0] == 0x01 && out[1] == 0x02 && out[2] == 0x03);
    out[0] = 0;
    assert(aegis_buffer_read(buf, out, 3, 5) == 0);
    assert(memcmp(out, "hello", 5) == 0);

    /* Out of bounds read */
    assert(aegis_buffer_read(buf, out, 100, 1) != 0);

    /* Clear retains capacity */
    aegis_buffer_clear(buf);
    assert(aegis_buffer_len(buf) == 0);
    assert(aegis_buffer_capacity(buf) > 0);

    /* Reserve */
    assert(aegis_buffer_reserve(buf, 1024) == 0);
    assert(aegis_buffer_capacity(buf) >= 1024);

    aegis_buffer_destroy(buf);
    aegis_buffer_destroy(NULL); /* safe */
    assert(aegis_buffer_create(NULL, 0) != 0);

    return 0;
}
