#include "aegis/common/list.h"
#include <assert.h>
#include <stdlib.h>

static void accumulate(void* item, void* ctx)
{
    int* sum = (int*)ctx;
    *sum += *(int*)item;
}

int main(void)
{
    aegis_list_t* list = NULL;
    assert(aegis_list_create(&list) == 0);
    assert(aegis_list_is_empty(list));
    assert(aegis_list_len(list) == 0);

    /* Push back */
    int* a = malloc(sizeof(int));
    *a     = 1;
    int* b = malloc(sizeof(int));
    *b     = 2;
    int* c = malloc(sizeof(int));
    *c     = 3;
    assert(aegis_list_push_back(list, a) == 0);
    assert(aegis_list_push_back(list, b) == 0);
    assert(aegis_list_push_back(list, c) == 0);
    assert(aegis_list_len(list) == 3);
    assert(!aegis_list_is_empty(list));

    /* Front / Back */
    const void* front = NULL;
    const void* back  = NULL;
    assert(aegis_list_front(list, &front) == 0);
    assert(*(int*)front == 1);
    assert(aegis_list_back(list, &back) == 0);
    assert(*(int*)back == 3);

    /* Pop front */
    int* val = NULL;
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 1);
    free(val); /* a */

    /* Pop back */
    assert(aegis_list_pop_back(list, (void**)&val) == 0);
    assert(*val == 3);
    free(val); /* c */
    assert(aegis_list_len(list) == 1);

    /* Push front */
    int* d = malloc(sizeof(int));
    *d     = 10;
    int* e = malloc(sizeof(int));
    *e     = 20;
    assert(aegis_list_push_front(list, d) == 0);
    assert(aegis_list_push_front(list, e) == 0);
    assert(aegis_list_len(list) == 3);

    assert(aegis_list_front(list, &front) == 0);
    assert(*(int*)front == 20);
    assert(aegis_list_back(list, &back) == 0);
    assert(*(int*)back == 2);

    /* for_each */
    int sum = 0;
    aegis_list_for_each(list, accumulate, &sum);
    assert(sum == 32); /* 20 + 10 + 2 */

    /* Pop all */
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 20);
    free(val); /* e */
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 10);
    free(val); /* d */
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 2);
    free(val); /* b */
    assert(aegis_list_is_empty(list));
    assert(aegis_list_len(list) == 0);

    /* Underflow */
    assert(aegis_list_pop_front(list, &val) != 0);
    assert(aegis_list_pop_back(list, &val) != 0);
    assert(aegis_list_front(list, &front) != 0);
    assert(aegis_list_back(list, &back) != 0);

    /* Push fresh items */
    int* f = malloc(sizeof(int));
    *f     = 4;
    int* g = malloc(sizeof(int));
    *g     = 5;
    assert(aegis_list_push_back(list, f) == 0);
    assert(aegis_list_push_front(list, g) == 0);
    assert(aegis_list_len(list) == 2);
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 5);
    free(val); /* g */
    assert(aegis_list_pop_back(list, (void**)&val) == 0);
    assert(*val == 4);
    free(val); /* f */
    assert(aegis_list_is_empty(list));

    /* Single element edge cases */
    int* single = malloc(sizeof(int));
    *single     = 42;
    assert(aegis_list_push_back(list, single) == 0);
    assert(aegis_list_len(list) == 1);
    assert(aegis_list_front(list, &front) == 0);
    assert(*(int*)front == 42);
    assert(aegis_list_back(list, &back) == 0);
    assert(*(int*)back == 42);
    assert(aegis_list_pop_front(list, (void**)&val) == 0);
    assert(*val == 42);
    free(val);
    assert(aegis_list_is_empty(list));

    /* Null/invalid calls */
    aegis_list_destroy(NULL);
    aegis_list_destroy(list);
    assert(aegis_list_create(NULL) != 0);

    /* Destroy does not free items */
    int* leaked = malloc(sizeof(int));
    *leaked     = 99;
    assert(aegis_list_create(&list) == 0);
    assert(aegis_list_push_back(list, leaked) == 0);
    aegis_list_destroy(list);
    assert(*leaked == 99); /* still valid memory */
    free(leaked);
    return 0;
}
