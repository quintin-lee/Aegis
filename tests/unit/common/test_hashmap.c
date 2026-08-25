#include "aegis/common/hashmap.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    aegis_hashmap_t* map = NULL;
    assert(aegis_hashmap_create(&map, 16, aegis_hash_fnv1a, aegis_eq_bytes, 0) == 0);
    assert(aegis_hashmap_is_empty(map));
    assert(aegis_hashmap_len(map) == 0);

    /* Insert */
    const char* k1 = "hello";
    int         v1 = 42;
    assert(aegis_hashmap_insert(map, k1, strlen(k1) + 1, &v1) == 0);
    assert(aegis_hashmap_len(map) == 1);

    /* Lookup */
    int* got = NULL;
    assert(aegis_hashmap_get(map, k1, strlen(k1) + 1, (void**)&got) == true);
    assert(got != NULL && *got == 42);

    /* Update existing: replace value pointer */
    int v1_new = 99;
    assert(aegis_hashmap_insert(map, k1, strlen(k1) + 1, &v1_new) == 0);
    /* Re-lookup to get new pointer */
    assert(aegis_hashmap_get(map, k1, strlen(k1) + 1, (void**)&got) == true);
    assert(got != NULL && *got == 99);

    /* Second entry */
    const char* k2 = "world";
    int         v2 = 7;
    assert(aegis_hashmap_insert(map, k2, strlen(k2) + 1, &v2) == 0);
    assert(aegis_hashmap_len(map) == 2);

    /* Get second */
    int* got2 = NULL;
    assert(aegis_hashmap_get(map, k2, strlen(k2) + 1, (void**)&got2) == true);
    assert(got2 != NULL && *got2 == 7);

    /* Remove k1 */
    assert(aegis_hashmap_remove(map, k1, strlen(k1) + 1) == true);
    assert(aegis_hashmap_len(map) == 1);
    assert(aegis_hashmap_get(map, k1, strlen(k1) + 1, (void**)&got) == false);

    /* k2 still present */
    assert(aegis_hashmap_get(map, k2, strlen(k2) + 1, (void**)&got2) == true);
    assert(got2 != NULL && *got2 == 7);

    /* Clear */
    aegis_hashmap_clear(map);
    assert(aegis_hashmap_is_empty(map));
    assert(aegis_hashmap_len(map) == 0);

    aegis_hashmap_destroy(map);
    assert(aegis_hashmap_len(NULL) == 0);
    assert(aegis_hashmap_is_empty(NULL) == true);

    return 0;
}
