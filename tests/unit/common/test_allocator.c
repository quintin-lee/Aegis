#include "aegis/common/allocator.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    const aegis_allocator_t* def = aegis_alloc_default();
    assert(def != NULL);

    /* Basic alloc/free via wrapper */
    void* p = aegis_alloc(def, 256);
    assert(p != NULL);
    memset(p, 0, 256);
    aegis_free(def, p);

    /* realloc */
    p = aegis_alloc(def, 64);
    assert(p != NULL);
    p = aegis_realloc(def, p, 64, 128);
    assert(p != NULL);
    aegis_free(def, p);

    /* Stats on default allocator are no-op */
    aegis_alloc_stats_t st;
    aegis_alloc_stats(def, &st);

    /* Tracking allocator — wrap default */
    aegis_allocator_t tracker = aegis_alloc_tracker(def);
    assert(tracker.alloc != NULL);
    void* q = tracker.alloc(&tracker, 1024, tracker.ctx);
    assert(q != NULL);
    memset(q, 0xAB, 1024);
    tracker.free(&tracker, q, tracker.ctx);

    aegis_alloc_stats_t tst;
    tracker.stats(&tracker, &tst, tracker.ctx);
    assert(tst.allocations >= 1);
    assert(tst.bytes_allocated >= 1024);

    /* Destroy tracker — should not crash */
    tracker = aegis_alloc_tracker_destroy(tracker);

    /* NULL alloc/free is safe */
    aegis_free(NULL, NULL);

    return 0;
}
