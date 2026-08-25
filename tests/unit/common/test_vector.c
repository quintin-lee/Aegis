#include "aegis/common/vector.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    /* --- create / destroy --- */
    aegis_vector_t* v = NULL;
    assert(aegis_vector_create(&v, sizeof(int)) == 0);
    assert(v != NULL);
    assert(aegis_vector_len(v) == 0);
    assert(aegis_vector_is_empty(v) == true);

    /* invalid create */
    assert(aegis_vector_create(NULL, sizeof(int)) != 0);
    aegis_vector_t* bad = NULL;
    assert(aegis_vector_create(&bad, 0) != 0);
    assert(bad == NULL);

    /* --- push / pop --- */
    for (int i = 0; i < 10; i++) {
        assert(aegis_vector_push(v, &i) == 0);
    }
    assert(aegis_vector_len(v) == 10);
    assert(aegis_vector_is_empty(v) == false);

    int last = -1;
    assert(aegis_vector_pop(v, &last) == 0);
    assert(last == 9);
    assert(aegis_vector_len(v) == 9);

    /* pop from empty */
    aegis_vector_t* empty = NULL;
    assert(aegis_vector_create(&empty, sizeof(int)) == 0);
    assert(aegis_vector_pop(empty, &last) != 0);
    aegis_vector_destroy(empty);

    /* --- get / set --- */
    int val = 0;
    assert(aegis_vector_get(v, 0, &val) == 0);
    assert(val == 0);
    assert(aegis_vector_get(v, 8, &val) == 0);
    assert(val == 8);

    /* get out of bounds */
    assert(aegis_vector_get(v, 9, &val) != 0);

    int new_val = 42;
    assert(aegis_vector_set(v, 0, &new_val) == 0);
    assert(aegis_vector_get(v, 0, &val) == 0);
    assert(val == 42);

    /* set out of bounds */
    assert(aegis_vector_set(v, 9, &new_val) != 0);

    /* --- clear (retain capacity) --- */
    aegis_vector_t* big = NULL;
    assert(aegis_vector_create(&big, sizeof(int)) == 0);
    for (int i = 0; i < 100; i++) {
        assert(aegis_vector_push(big, &i) == 0);
    }
    aegis_vector_clear(big);
    assert(aegis_vector_len(big) == 0);
    assert(aegis_vector_is_empty(big) == true);
    /* capacity should still allow 100 pushes without realloc */
    for (int i = 0; i < 100; i++) {
        assert(aegis_vector_push(big, &i) == 0);
    }
    assert(aegis_vector_len(big) == 100);
    aegis_vector_destroy(big);

    /* --- reserve --- */
    aegis_vector_t* rv = NULL;
    assert(aegis_vector_create(&rv, sizeof(int)) == 0);
    assert(aegis_vector_reserve(rv, 200) == 0);
    for (int i = 0; i < 200; i++) {
        assert(aegis_vector_push(rv, &i) == 0);
    }
    assert(aegis_vector_len(rv) == 200);
    aegis_vector_destroy(rv);

    /* --- destroy NULL safe --- */
    aegis_vector_destroy(NULL);
    aegis_vector_destroy(v);

    /* --- different element sizes --- */
    aegis_vector_t* st = NULL;
    assert(aegis_vector_create(&st, sizeof(double)) == 0);
    double d = 3.14;
    assert(aegis_vector_push(st, &d) == 0);
    double got = 0.0;
    assert(aegis_vector_get(st, 0, &got) == 0);
    assert(fabs(got - 3.14) < 1e-10);
    aegis_vector_destroy(st);

    return 0;
}
