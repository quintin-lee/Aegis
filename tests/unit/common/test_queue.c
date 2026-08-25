#include "aegis/common/queue.h"
#include <assert.h>
#include <stdlib.h>

int main(void)
{
    aegis_queue_t* q = NULL;
    assert(aegis_queue_create(&q, 4) == 0);
    assert(aegis_queue_is_empty(q));

    /* Push 4 ints via pointers */
    int* items[4];
    for (int i = 0; i < 4; i++) {
        items[i] = malloc(sizeof(int));
        assert(items[i] != NULL);
        *items[i] = i + 1;
        assert(aegis_queue_push(q, items[i]) == 0);
    }
    assert(aegis_queue_len(q) == 4);
    assert(aegis_queue_is_full(q));

    /* Peek front — returns pointer */
    int* peek = NULL;
    assert(aegis_queue_peek(q, (void**)&peek) == 0);
    assert(peek != NULL && *peek == 1);

    /* Pop all, free each */
    int* val;
    assert(aegis_queue_pop(q, (void**)&val) == 0);
    assert(*val == 1);
    free(val);
    assert(aegis_queue_pop(q, (void**)&val) == 0);
    assert(*val == 2);
    free(val);
    assert(aegis_queue_pop(q, (void**)&val) == 0);
    assert(*val == 3);
    free(val);
    assert(aegis_queue_pop(q, (void**)&val) == 0);
    assert(*val == 4);
    free(val);
    assert(aegis_queue_is_empty(q));

    /* Underflow */
    assert(aegis_queue_pop(q, &val) != 0);

    /* Fill fresh with new allocations */
    int* w = malloc(sizeof(int));
    *w     = 10;
    int* x = malloc(sizeof(int));
    *x     = 20;
    int* y = malloc(sizeof(int));
    *y     = 30;
    int* z = malloc(sizeof(int));
    *z     = 40;
    assert(aegis_queue_push(q, w) == 0);
    assert(aegis_queue_push(q, x) == 0);
    assert(aegis_queue_push(q, y) == 0);
    assert(aegis_queue_push(q, z) == 0);
    assert(aegis_queue_is_full(q));

    /* Overflow — fresh item */
    int* overflow = malloc(sizeof(int));
    *overflow     = 99;
    assert(aegis_queue_push(q, overflow) != 0);
    free(overflow);

    /* Clear does NOT free items */
    aegis_queue_clear(q);
    assert(aegis_queue_is_empty(q));
    free(w);
    free(x);
    free(y);
    free(z);

    aegis_queue_destroy(q);
    aegis_queue_destroy(NULL);
    assert(aegis_queue_create(NULL, 4) != 0);
    assert(aegis_queue_create(&q, 3) != 0);

    return 0;
}
