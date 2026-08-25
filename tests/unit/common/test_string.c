#include "aegis/common/string.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    aegis_string_t* s = NULL;
    assert(aegis_string_create(&s) == 0);
    assert(aegis_string_is_empty(s));
    aegis_string_destroy(s);

    /* From C string */
    assert(aegis_string_from_cstr(&s, "hello world") == 0);
    assert(strcmp(aegis_string_cstr(s), "hello world") == 0);
    assert(aegis_string_len(s) == 11);
    assert(!aegis_string_is_empty(s));

    /* Append */
    aegis_string_t* s2 = NULL;
    assert(aegis_string_from_cstr(&s2, " foo") == 0);
    assert(aegis_string_append(s, s2) == 0);
    assert(strcmp(aegis_string_cstr(s), "hello world foo") == 0);
    aegis_string_destroy(s2);

    /* Equality */
    aegis_string_t* s3 = NULL;
    assert(aegis_string_from_cstr(&s3, "hello world foo") == 0);
    assert(aegis_string_eq(s, s3));
    aegis_string_destroy(s3);

    /* Substring */
    aegis_string_t* sub = NULL;
    assert(aegis_string_substring(s, 0, 5, &sub) == 0);
    assert(strcmp(aegis_string_cstr(sub), "hello") == 0);
    aegis_string_destroy(sub);

    aegis_string_destroy(s);

    /* NULL safety */
    assert(aegis_string_len(NULL) == 0);
    assert(aegis_string_is_empty(NULL) == true);
    assert(aegis_string_eq(NULL, NULL) == true);
    assert(aegis_string_create(NULL) != 0);

    return 0;
}
