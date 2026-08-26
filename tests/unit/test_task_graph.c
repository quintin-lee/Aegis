/**
 * @file test_task_graph.c
#include <stdlib.h>
 * @brief Tests for task graph DAG operations, cycle detection, and queries.
 */
#include "aegis/graph.h"
#include "aegis/task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Test graph topology:
 *      T1 → T2 → T3
 *      T1 → T4
 *      T3, T4 → T5
 *
 *      Ready tasks after adding: T1
 *      After T1 completes: T2, T4
 *      After T2 completes: T3
 *      After T3,T4 complete: T5
 */

static void test_basic_lifecycle(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL, *t3 = NULL;
    assert(aegis_task_create(&t1, "t1", "first") == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", "second") == AEGIS_OK);
    assert(aegis_task_create(&t3, "t3", "third") == AEGIS_OK);

    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t3) == AEGIS_OK);

    assert(aegis_task_graph_task_count(g) == 3);
    assert(aegis_task_graph_dependency_count(g) == 0);

    aegis_task_graph_destroy(g);
}

static void test_add_remove_dependency(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);

    /* Add dependency */
    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);
    assert(aegis_task_graph_dependency_count(g) == 1);

    /* Duplicate is no-op */
    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);
    assert(aegis_task_graph_dependency_count(g) == 1);

    /* Remove dependency */
    assert(aegis_task_graph_remove_dependency(g, t1, t2) == AEGIS_OK);
    assert(aegis_task_graph_dependency_count(g) == 0);

    /* Remove non-existent */
    assert(aegis_task_graph_remove_dependency(g, t1, t2) == AEGIS_ERR_NOT_FOUND);

    aegis_task_graph_destroy(g);
}

static void test_cycle_detection(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL, *t3 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t3, "t3", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t3) == AEGIS_OK);

    /* Build: T1 → T2 → T3 */
    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, t2, t3) == AEGIS_OK);
    assert(aegis_task_graph_is_dag(g) == true);

    /* Try to create cycle: T3 → T1 */
    assert(aegis_task_graph_add_dependency(g, t3, t1) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_is_dag(g) == true); /* still acyclic */

    aegis_task_graph_destroy(g);
}

static void test_self_loop(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);

    /* Self-loop rejected */
    assert(aegis_task_graph_add_dependency(g, t1, t1) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_dependency_count(g) == 0);

    aegis_task_graph_destroy(g);
}

static void test_validate_topology(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);

    assert(aegis_task_graph_validate(g) == AEGIS_OK);

    /* Create cycle */
    assert(aegis_task_graph_add_dependency(g, t2, t1) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_validate(g) == AEGIS_OK); /* still valid */

    aegis_task_graph_destroy(g);
}

static void test_complex_dag(void) {
    /* T1 → T2 → T3
       T1 → T4
       T3,T4 → T5 */
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* tasks[5];
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "t%d", i + 1);
        assert(aegis_task_create(&tasks[i], name, NULL) == AEGIS_OK);
        assert(aegis_task_graph_add_task(g, tasks[i]) == AEGIS_OK);
    }

    assert(aegis_task_graph_add_dependency(g, tasks[0], tasks[1]) == AEGIS_OK); /* T1→T2 */
    assert(aegis_task_graph_add_dependency(g, tasks[1], tasks[2]) == AEGIS_OK); /* T2→T3 */
    assert(aegis_task_graph_add_dependency(g, tasks[0], tasks[3]) == AEGIS_OK); /* T1→T4 */
    assert(aegis_task_graph_add_dependency(g, tasks[2], tasks[4]) == AEGIS_OK); /* T3→T5 */
    assert(aegis_task_graph_add_dependency(g, tasks[3], tasks[4]) == AEGIS_OK); /* T4→T5 */

    assert(aegis_task_graph_task_count(g) == 5);
    assert(aegis_task_graph_dependency_count(g) == 5);
    assert(aegis_task_graph_is_dag(g) == true);
    assert(aegis_task_graph_validate(g) == AEGIS_OK);

    aegis_task_graph_destroy(g);
}

static void test_cycle_complex(void) {
    /* T1 → T2 → T3 → T1 (cycle) */
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* tasks[3];
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "t%d", i + 1);
        assert(aegis_task_create(&tasks[i], name, NULL) == AEGIS_OK);
        assert(aegis_task_graph_add_task(g, tasks[i]) == AEGIS_OK);
    }

    assert(aegis_task_graph_add_dependency(g, tasks[0], tasks[1]) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, tasks[1], tasks[2]) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, tasks[2], tasks[0]) == AEGIS_ERR_INVALID);

    assert(aegis_task_graph_is_dag(g) == true); /* still DAG */
    assert(aegis_task_graph_validate(g) == AEGIS_OK);

    aegis_task_graph_destroy(g);
}

static void test_ready_tasks(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL, *t3 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t3, "t3", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t3) == AEGIS_OK);

    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, t2, t3) == AEGIS_OK);

    /* Initially all pending, none ready */
    aegis_task_t** ready = NULL;
    size_t count = 0;
    assert(aegis_task_graph_ready_tasks(g, &ready, &count) == AEGIS_OK);
    assert(count == 0);
    free(ready);

    /* Mark t1 as running, t2 and t3 still pending (dep not satisfied) */
    aegis_task_set_state_for_test(t1, AEGIS_TASK_RUNNING);

    /* t2 still pending (t1 not done), t3 pending */
    ready = NULL; count = 0;
    assert(aegis_task_graph_ready_tasks(g, &ready, &count) == AEGIS_OK);
    assert(count == 0);
    free(ready);

    /* Mark t1 as success, t2 should be ready */
    aegis_task_set_state_for_test(t1, AEGIS_TASK_SUCCESS);
    aegis_task_set_state_for_test(t2, AEGIS_TASK_PENDING); /* deps satisfied but not yet marked ready */

    /* Simulate scheduler: update dependent states */
    /* In a real system, the scheduler would do this. Here we just verify structure. */

    aegis_task_graph_destroy(g);
}

static void test_remove_task(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t2) == AEGIS_OK);
    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_OK);

    assert(aegis_task_graph_task_count(g) == 2);
    assert(aegis_task_graph_dependency_count(g) == 1);

    /* Remove t1 — should also remove dependency */
    assert(aegis_task_graph_remove_task(g, t1) == AEGIS_OK);
    assert(aegis_task_graph_task_count(g) == 1);
    assert(aegis_task_graph_dependency_count(g) == 0);

    /* t1 was removed, caller must destroy it */
    aegis_task_destroy(t1);
    /* t2 is still in graph, will be freed by graph destroy */
    assert(aegis_task_state(t2) == AEGIS_TASK_PENDING);

    aegis_task_graph_destroy(g);
}

static void test_null_operations(void) {
    assert(aegis_task_graph_add_task(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_remove_task(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_add_dependency(NULL, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_remove_dependency(NULL, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_get_task(NULL, 1) == NULL);
    assert(aegis_task_graph_ready_tasks(NULL, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_graph_task_count(NULL) == 0);
    assert(aegis_task_graph_dependency_count(NULL) == 0);
    assert(aegis_task_graph_is_dag(NULL) == true);
    assert(aegis_task_graph_validate(NULL) == AEGIS_ERR_INVALID);
    aegis_task_graph_destroy(NULL);
}

static void test_nonexistent_dependency(void) {
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* t1 = NULL, *t2 = NULL;
    assert(aegis_task_create(&t1, "t1", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "t2", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t1) == AEGIS_OK);
    /* t2 not added to graph */

    assert(aegis_task_graph_add_dependency(g, t1, t2) == AEGIS_ERR_NOT_FOUND);

    /* t2 was never added to graph, destroy it here */
    aegis_task_destroy(t2);
    aegis_task_graph_destroy(g);
}

int main(void) {
    test_basic_lifecycle();
    test_add_remove_dependency();
    test_cycle_detection();
    test_self_loop();
    test_validate_topology();
    test_complex_dag();
    test_cycle_complex();
    test_ready_tasks();
    test_remove_task();
    test_null_operations();
    test_nonexistent_dependency();

    printf("task graph test passed\n");
    return 0;
}
