/**
 * @file test_memory.c
 * @brief Unit tests for the Memory module:
 *   - Generic memory store: create/destroy, put/get/remove/search
 *   - Working memory: capacity eviction, top-N query
 *   - Episodic memory: append, range query
 *   - Semantic memory: put/get (overwrite), duplicate ids
 *   - Procedural memory: put/search by keyword
 *   - Null safety
 */
#include "aegis/memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _flush(void) { setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); }

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_memory_item_t* make_item(const char* id, const char* content,
                                      aegis_memory_item_type_t type, int priority)
{
    aegis_memory_item_t* item = calloc(1, sizeof(*item));
    assert(item);
    item->id         = strdup(id);
    item->content    = strdup(content);
    item->type       = type;
    item->priority   = priority;
    item->timestamp  = 0;
    assert(item->id && item->content);
    return item;
}

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Generic memory ────────────────────────────────────────────────────────── */

static void test_generic_put_get(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");

    aegis_memory_item_t* item = make_item("id1", "hello world",
                                          AEGIS_MEMORY_ITEM_GOAL, 5);
    expect_ok(aegis_memory_put(mem, item), "put");
    assert(aegis_memory_count(mem) == 1);

    aegis_memory_item_t* found = NULL;
    expect_ok(aegis_memory_get(mem, "id1", &found), "get");
    assert(found != NULL);
    assert(strcmp(found->content, "hello world") == 0);
    assert(found->type == AEGIS_MEMORY_ITEM_GOAL);

    /* item transferred to mem, found is borrowed — do not free either. */
    aegis_memory_destroy(mem);
}

static void test_generic_remove(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");

    aegis_memory_item_t* item = make_item("x", "data", AEGIS_MEMORY_ITEM_PLAN, 3);
    expect_ok(aegis_memory_put(mem, item), "put");
    assert(aegis_memory_count(mem) == 1);

    aegis_memory_item_t* removed = NULL;
    expect_ok(aegis_memory_remove(mem, "x", &removed), "remove");
    assert(removed != NULL);
    assert(strcmp(removed->content, "data") == 0);
    assert(aegis_memory_count(mem) == 0);
    aegis_memory_item_destroy(removed); /* caller owns removed item */

    aegis_memory_destroy(mem);
}

static void test_generic_remove_not_found(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");
    aegis_memory_item_t* out = NULL;
    assert(aegis_memory_remove(mem, "nope", &out) == AEGIS_ERR_NOT_FOUND);
    aegis_memory_destroy(mem);
}

static void test_generic_search_by_type(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");

    aegis_memory_item_t* g1 = make_item("g1", "goal one", AEGIS_MEMORY_ITEM_GOAL, 10);
    aegis_memory_item_t* g2 = make_item("g2", "goal two", AEGIS_MEMORY_ITEM_GOAL, 5);
    aegis_memory_item_t* p  = make_item("p",  "plan",     AEGIS_MEMORY_ITEM_PLAN, 3);
    expect_ok(aegis_memory_put(mem, g1), "put g1");
    expect_ok(aegis_memory_put(mem, g2), "put g2");
    expect_ok(aegis_memory_put(mem, p),  "put p");

    aegis_memory_item_t** matches = NULL;
    size_t count = 0;
    expect_ok(aegis_memory_search_by_type(mem, AEGIS_MEMORY_ITEM_GOAL, &matches, &count),
              "search goals");
    assert(count == 2);
    assert(matches != NULL);
    free(matches);

    expect_ok(aegis_memory_search_by_type(mem, AEGIS_MEMORY_ITEM_PLAN, &matches, &count),
              "search plans");
    assert(count == 1);
    free(matches);

    /* Generic matches all. */
    expect_ok(aegis_memory_search_by_type(mem, AEGIS_MEMORY_ITEM_GENERIC, &matches, &count),
              "search generic");
    assert(count == 3);
    free(matches);

    aegis_memory_destroy(mem);
}

static void test_generic_overwrite(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");

    aegis_memory_item_t* item = make_item("id1", "v1", AEGIS_MEMORY_ITEM_GOAL, 1);
    expect_ok(aegis_memory_put(mem, item), "put v1");

    aegis_memory_item_t* updated = make_item("id1", "v2", AEGIS_MEMORY_ITEM_GOAL, 2);
    expect_ok(aegis_memory_put(mem, updated), "put v2");

    assert(aegis_memory_count(mem) == 1);
    aegis_memory_item_t* found = NULL;
    expect_ok(aegis_memory_get(mem, "id1", &found), "get");
    assert(strcmp(found->content, "v2") == 0);
    assert(found->priority == 2);

    aegis_memory_destroy(mem);
}

/* ── Working memory ────────────────────────────────────────────────────────── */

static void test_working_capacity_eviction(void)
{
    aegis_working_memory_t* wm = NULL;
    expect_ok(aegis_working_memory_create(&wm, 3), "create");

    /* Insert 4 items with priorities 1,2,3,4 — lowest (1) should be evicted. */
    aegis_memory_item_t* a = make_item("a", "low",  AEGIS_MEMORY_ITEM_GOAL, 1);
    aegis_memory_item_t* b = make_item("b", "mid",  AEGIS_MEMORY_ITEM_PLAN, 2);
    aegis_memory_item_t* c = make_item("c", "high", AEGIS_MEMORY_ITEM_GOAL, 3);
    aegis_memory_item_t* d = make_item("d", "top",  AEGIS_MEMORY_ITEM_PLAN, 4);
    expect_ok(aegis_working_memory_put(wm, a), "put a");
    expect_ok(aegis_working_memory_put(wm, b), "put b");
    expect_ok(aegis_working_memory_put(wm, c), "put c");
    expect_ok(aegis_working_memory_put(wm, d), "put d");

    assert(aegis_working_memory_count(wm) == 3);

    /* Top-2 should be d (4) and c (3). */
    aegis_memory_item_t** top = NULL;
    size_t top_count = 0;
    expect_ok(aegis_working_memory_top(wm, 2, &top, &top_count), "top 2");
    assert(top_count == 2);
    assert(strcmp(top[0]->id, "d") == 0);
    assert(strcmp(top[1]->id, "c") == 0);
    free(top);

    aegis_working_memory_destroy(wm);
}

static void test_working_no_capacity_limit(void)
{
    aegis_working_memory_t* wm = NULL;
    expect_ok(aegis_working_memory_create(&wm, 0), "create unlimited");

    for (int i = 0; i < 10; i++) {
        char id[16];
        snprintf(id, sizeof(id), "item%d", i);
        char content[32];
        snprintf(content, sizeof(content), "content %d", i);
        aegis_memory_item_t* item = make_item(id, content,
                                              AEGIS_MEMORY_ITEM_GOAL, i);
        expect_ok(aegis_working_memory_put(wm, item), "put item");
    }
    assert(aegis_working_memory_count(wm) == 10);
    aegis_working_memory_destroy(wm);
}

/* ── Episodic memory ───────────────────────────────────────────────────────── */

static void test_episodic_append_range(void)
{
    aegis_episodic_memory_t* em = NULL;
    expect_ok(aegis_episodic_memory_create(&em), "create");

    aegis_memory_item_t* e1 = make_item("e1", "first",  AEGIS_MEMORY_ITEM_OBSERVATION, 1);
    e1->timestamp = 1000;
    aegis_memory_item_t* e2 = make_item("e2", "second", AEGIS_MEMORY_ITEM_OBSERVATION, 2);
    e2->timestamp = 2000;
    aegis_memory_item_t* e3 = make_item("e3", "third",  AEGIS_MEMORY_ITEM_OBSERVATION, 3);
    e3->timestamp = 3000;
    expect_ok(aegis_episodic_memory_append(em, e1), "append e1");
    expect_ok(aegis_episodic_memory_append(em, e2), "append e2");
    expect_ok(aegis_episodic_memory_append(em, e3), "append e3");

    assert(aegis_episodic_memory_count(em) == 3);

    /* Range [1500, 2500) should return only e2. */
    aegis_memory_item_t** events = NULL;
    size_t count = 0;
    expect_ok(aegis_episodic_memory_range(em, 1500, 2500, &events, &count), "range");
    assert(count == 1);
    assert(strcmp(events[0]->id, "e2") == 0);
    free(events);

    /* Full range. */
    expect_ok(aegis_episodic_memory_range(em, 0, 10000, &events, &count), "full range");
    assert(count == 3);
    free(events);

    aegis_episodic_memory_destroy(em);
}

/* ── Semantic memory ───────────────────────────────────────────────────────── */

static void test_semantic_overwrite(void)
{
    aegis_semantic_memory_t* sm = NULL;
    expect_ok(aegis_semantic_memory_create(&sm), "create");

    aegis_memory_item_t* f1 = make_item("fact1", "old fact", AEGIS_MEMORY_ITEM_KNOWLEDGE, 1);
    expect_ok(aegis_semantic_memory_put(sm, f1), "put f1");

    aegis_memory_item_t* f2 = make_item("fact1", "new fact", AEGIS_MEMORY_ITEM_KNOWLEDGE, 2);
    expect_ok(aegis_semantic_memory_put(sm, f2), "put f2");

    assert(aegis_semantic_memory_count(sm) == 1);
    aegis_memory_item_t* found = NULL;
    expect_ok(aegis_semantic_memory_get(sm, "fact1", &found), "get");
    assert(strcmp(found->content, "new fact") == 0);

    aegis_semantic_memory_destroy(sm);
}

static void test_semantic_not_found(void)
{
    aegis_semantic_memory_t* sm = NULL;
    expect_ok(aegis_semantic_memory_create(&sm), "create");
    aegis_memory_item_t* out = NULL;
    assert(aegis_semantic_memory_get(sm, "missing", &out) == AEGIS_ERR_NOT_FOUND);
    aegis_semantic_memory_destroy(sm);
}

/* ── Procedural memory ───────────────────────────────────────────────────────── */

static void test_procedural_search(void)
{
    aegis_procedural_memory_t* pm = NULL;
    expect_ok(aegis_procedural_memory_create(&pm), "create");

    aegis_memory_item_t* ex1 = make_item("ex1", "use hash map for caching",
                                         AEGIS_MEMORY_ITEM_EXPERIENCE, 1);
    aegis_memory_item_t* ex2 = make_item("ex2", "avoid deep recursion",
                                         AEGIS_MEMORY_ITEM_EXPERIENCE, 2);
    aegis_memory_item_t* ex3 = make_item("ex3", "hash map lookup is fast",
                                         AEGIS_MEMORY_ITEM_EXPERIENCE, 3);
    expect_ok(aegis_procedural_memory_put(pm, ex1), "put ex1");
    expect_ok(aegis_procedural_memory_put(pm, ex2), "put ex2");
    expect_ok(aegis_procedural_memory_put(pm, ex3), "put ex3");

    /* Search for "hash" should match ex1 and ex3. */
    aegis_memory_item_t** results = NULL;
    size_t count = 0;
    expect_ok(aegis_procedural_memory_search(pm, "hash", &results, &count), "search hash");
    assert(count == 2);
    free(results);

    /* Search for nonexistent. */
    expect_ok(aegis_procedural_memory_search(pm, "zzzzz", &results, &count), "search zzz");
    assert(count == 0);
    assert(results == NULL);

    aegis_procedural_memory_destroy(pm);
}

/* ── Null safety ───────────────────────────────────────────────────────────── */

static void test_null_safe_api(void)
{
    aegis_memory_destroy(NULL);
    aegis_memory_item_destroy(NULL);
    aegis_working_memory_destroy(NULL);
    aegis_episodic_memory_destroy(NULL);
    aegis_semantic_memory_destroy(NULL);
    aegis_procedural_memory_destroy(NULL);

    assert(aegis_memory_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_working_memory_create(NULL, 10) == AEGIS_ERR_INVALID);
    assert(aegis_episodic_memory_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_semantic_memory_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_procedural_memory_create(NULL) == AEGIS_ERR_INVALID);
}

/* ── Invalid args ──────────────────────────────────────────────────────────── */

static void test_invalid_args(void)
{
    aegis_memory_t* mem = NULL;
    expect_ok(aegis_memory_create(&mem), "create");

    assert(aegis_memory_put(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_memory_put(mem, NULL) == AEGIS_ERR_INVALID);

    /* Empty id. */
    aegis_memory_item_t* bad = calloc(1, sizeof(*bad));
    assert(bad);
    bad->id      = strdup("");
    bad->content = strdup("data");
    assert(bad->id && bad->content);
    assert(aegis_memory_put(mem, bad) == AEGIS_ERR_INVALID);
    free(bad->id);
    free(bad->content);
    free(bad);

    /* NULL content (manually constructed, not via make_item). */
    aegis_memory_item_t* no_content = calloc(1, sizeof(*no_content));
    assert(no_content);
    no_content->id = strdup("id");
    assert(no_content->id);
    /* content stays NULL — should be rejected by aegis_memory_put. */
    assert(aegis_memory_put(mem, no_content) == AEGIS_ERR_INVALID);
    free(no_content->id);
    free(no_content);

    aegis_memory_item_t* out = NULL;
    assert(aegis_memory_get(NULL, "x", &out) == AEGIS_ERR_INVALID);
    assert(aegis_memory_get(mem, NULL, &out) == AEGIS_ERR_INVALID);
    assert(aegis_memory_get(mem, "x", NULL) == AEGIS_ERR_INVALID);

    aegis_memory_destroy(mem);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    _flush();
    printf("starting null_safe_api();... "); fflush(stdout); test_null_safe_api();; printf("ok\n");
    printf("starting invalid_args();... "); fflush(stdout); test_invalid_args();; printf("ok\n");
    printf("starting generic_put_get();... "); fflush(stdout); test_generic_put_get();; printf("ok\n");
    printf("starting generic_remove();... "); fflush(stdout); test_generic_remove();; printf("ok\n");
    printf("starting generic_remove_not_found();... "); fflush(stdout); test_generic_remove_not_found();; printf("ok\n");
    printf("starting generic_search_by_type();... "); fflush(stdout); test_generic_search_by_type();; printf("ok\n");
    printf("starting generic_overwrite();... "); fflush(stdout); test_generic_overwrite();; printf("ok\n");
    printf("starting working_capacity_eviction();... "); fflush(stdout); test_working_capacity_eviction();; printf("ok\n");
    printf("starting working_no_capacity_limit();... "); fflush(stdout); test_working_no_capacity_limit();; printf("ok\n");
    printf("starting episodic_append_range();... "); fflush(stdout); test_episodic_append_range();; printf("ok\n");
    printf("starting semantic_overwrite();... "); fflush(stdout); test_semantic_overwrite();; printf("ok\n");
    printf("starting semantic_not_found();... "); fflush(stdout); test_semantic_not_found();; printf("ok\n");
    printf("starting procedural_search();... "); fflush(stdout); test_procedural_search();; printf("ok\n");

    printf("memory: all tests passed\n");
    return 0;
}
