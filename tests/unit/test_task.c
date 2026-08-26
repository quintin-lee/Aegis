/**
 * @file test_task.c
 * @brief Tests for task lifecycle, properties, and state.
 */
#include "aegis/task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_create_destroy(void) {
    aegis_task_t* task = NULL;
    assert(aegis_task_create(&task, "t1", "desc") == AEGIS_OK);
    assert(task != NULL);
    assert(aegis_task_id(task) > 0);
    assert(strcmp(aegis_task_name(task), "t1") == 0);
    assert(strcmp(aegis_task_description(task), "desc") == 0);
    assert(aegis_task_state(task) == AEGIS_TASK_PENDING);
    assert(aegis_task_type(task) == AEGIS_TASK_TYPE_CUSTOM);
    assert(aegis_task_priority(task) == 0);
    assert(aegis_task_timeout_ms(task) == 0);
    aegis_task_destroy(task);
}

static void test_null_create(void) {
    aegis_task_t* t = NULL;
    assert(aegis_task_create(NULL, "x", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_create(&t, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_task_create(&t, "", NULL) == AEGIS_ERR_INVALID);
}

static void test_null_operations(void) {
    assert(aegis_task_id(NULL) == 0);
    assert(aegis_task_name(NULL) == NULL);
    assert(aegis_task_description(NULL) == NULL);
    assert(aegis_task_state(NULL) == AEGIS_TASK_PENDING);
    assert(aegis_task_type(NULL) == AEGIS_TASK_TYPE_CUSTOM);
    assert(aegis_task_priority(NULL) == 0);
    assert(aegis_task_error(NULL) == NULL);
    assert(aegis_task_timeout_ms(NULL) == 0);
    assert(aegis_task_input(NULL, NULL) == NULL);
    assert(aegis_task_output(NULL, NULL) == NULL);
    assert(aegis_task_get_metadata(NULL, "k") == NULL);
    aegis_task_set_type(NULL, AEGIS_TASK_TYPE_COMPUTATIONAL);
    aegis_task_set_priority(NULL, 5);
    aegis_task_set_timeout_ms(NULL, 1000);
    aegis_task_remove_metadata(NULL, "k");
    aegis_task_destroy(NULL);
}

static void test_properties(void) {
    aegis_task_t* task = NULL;
    assert(aegis_task_create(&task, "prop", NULL) == AEGIS_OK);

    aegis_task_set_type(task, AEGIS_TASK_TYPE_IO);
    assert(aegis_task_type(task) == AEGIS_TASK_TYPE_IO);

    aegis_task_set_priority(task, 10);
    assert(aegis_task_priority(task) == 10);

    aegis_task_set_timeout_ms(task, 30000);
    assert(aegis_task_timeout_ms(task) == 30000);

    aegis_task_retry_policy_t policy = {3, 500, true};
    aegis_task_set_retry_policy(task, policy);
    aegis_task_retry_policy_t got = aegis_task_retry_policy(task);
    assert(got.max_attempts == 3);
    assert(got.delay_ms == 500);
    assert(got.exponential_backoff == true);

    aegis_task_destroy(task);
}

static void test_input_output(void) {
    aegis_task_t* task = NULL;
    assert(aegis_task_create(&task, "io", NULL) == AEGIS_OK);

    char data[] = "hello world";
    assert(aegis_task_set_input(task, data, sizeof(data)) == AEGIS_OK);
    size_t sz = 0;
    const void* inp = aegis_task_input(task, &sz);
    assert(inp != NULL);
    assert(sz == sizeof(data));
    assert(memcmp(inp, data, sizeof(data)) == 0);

    char out[] = "result";
    assert(aegis_task_set_output(task, out, sizeof(out)) == AEGIS_OK);
    sz = 0;
    const void* op = aegis_task_output(task, &sz);
    assert(op != NULL);
    assert(sz == sizeof(out));
    assert(memcmp(op, out, sizeof(out)) == 0);

    /* NULL input/output */
    assert(aegis_task_input(task, NULL) != NULL); // returns pointer even if size is NULL
    assert(aegis_task_output(task, NULL) != NULL); // returns pointer even if size is NULL

    aegis_task_destroy(task);
}

static void test_metadata(void) {
    aegis_task_t* task = NULL;
    assert(aegis_task_create(&task, "meta", NULL) == AEGIS_OK);

    assert(aegis_task_set_metadata(task, "key1", "value1") == AEGIS_OK);
    assert(strcmp(aegis_task_get_metadata(task, "key1"), "value1") == 0);
    assert(aegis_task_get_metadata(task, "missing") == NULL);

    /* Update existing */
    assert(aegis_task_set_metadata(task, "key1", "value2") == AEGIS_OK);
    assert(strcmp(aegis_task_get_metadata(task, "key1"), "value2") == 0);

    /* Remove */
    aegis_task_remove_metadata(task, "key1");
    assert(aegis_task_get_metadata(task, "key1") == NULL);

    /* Null key rejected */
    assert(aegis_task_set_metadata(task, NULL, "v") == AEGIS_ERR_INVALID);

    aegis_task_destroy(task);
}

static void test_id_uniqueness(void) {
    aegis_task_t* t1 = NULL,* t2 = NULL;
    assert(aegis_task_create(&t1, "a", NULL) == AEGIS_OK);
    assert(aegis_task_create(&t2, "b", NULL) == AEGIS_OK);
    assert(aegis_task_id(t1) != aegis_task_id(t2));
    aegis_task_destroy(t1);
    aegis_task_destroy(t2);
}

int main(void) {
    test_create_destroy();
    test_null_create();
    test_null_operations();
    test_properties();
    test_input_output();
    test_metadata();
    test_id_uniqueness();

    printf("task unit test passed\n");
    return 0;
}
