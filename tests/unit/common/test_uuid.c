#include "aegis/common/uuid.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    /* Null UUID */
    aegis_uuid_t null_u = aegis_uuid_null();
    assert(aegis_uuid_is_null(&null_u));

    aegis_uuid_t u1 = aegis_uuid_generate();
    assert(!aegis_uuid_is_null(&u1));

    /* Generate two distinct UUIDs */
    aegis_uuid_t u2 = aegis_uuid_generate();
    assert(!aegis_uuid_eq(&u1, &u2)); /* extremely likely distinct */

    /* Format */
    char buf[64];
    aegis_uuid_format(&u1, buf, sizeof(buf));
    assert(strlen(buf) == 36);
    assert(buf[8] == '-' && buf[13] == '-' && buf[18] == '-' && buf[23] == '-');

    /* Parse hyphenated */
    aegis_uuid_t parsed;
    assert(aegis_uuid_parse(buf, &parsed) == true);
    assert(aegis_uuid_eq(&u1, &parsed));

    /* Parse raw hex (strip dashes) */
    char raw[37] = {0};
    memcpy(raw, buf, 8);
    memcpy(raw + 8, buf + 9, 4);
    memcpy(raw + 12, buf + 14, 4);
    memcpy(raw + 16, buf + 19, 4);
    memcpy(raw + 20, buf + 24, 12);
    assert(aegis_uuid_parse(raw, &parsed) == true);
    assert(aegis_uuid_eq(&u1, &parsed));

    /* Invalid parse */
    assert(aegis_uuid_parse("not-a-uuid", &parsed) == false);
    assert(aegis_uuid_parse(NULL, NULL) == false);

    return 0;
}
