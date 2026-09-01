/**
 * @file test_message.c
 * @brief Unit tests for Message, ToolCall, ToolResult (Phase1)
 */
#include "aegis/message/message.h"
#include "aegis/message/role.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc);
        assert(0);
    }
}

static void test_role_str(void)
{
    assert(strcmp(aegis_message_role_str(AEGIS_MESSAGE_USER), "user") == 0);
    assert(strcmp(aegis_message_role_str(AEGIS_MESSAGE_ASSISTANT), "assistant") == 0);
    assert(strcmp(aegis_message_role_str(AEGIS_MESSAGE_TOOL), "tool") == 0);
    printf("role_str PASS\n");
}

static void test_tool_call_lifecycle(void)
{
    aegis_tool_call_t* c = NULL;
    expect_ok(aegis_tool_call_create(&c), "create");
    expect_ok(aegis_tool_call_set_name(c, "read"), "set_name");
    expect_ok(aegis_tool_call_set_id(c, "call_123"), "set_id");
    expect_ok(aegis_tool_call_set_arguments(c, "{\"path\":\"a.txt\"}"), "set_args");
    expect_ok(aegis_tool_call_set_index(c, 0), "set_index");
    assert(strcmp(aegis_tool_call_name(c), "read") == 0);
    assert(strcmp(aegis_tool_call_id(c), "call_123") == 0);
    assert(aegis_tool_call_index(c) == 0);
    aegis_tool_call_t* clone = NULL;
    expect_ok(aegis_tool_call_clone(c, &clone), "clone");
    assert(strcmp(aegis_tool_call_name(clone), "read") == 0);
    aegis_tool_call_destroy(clone);
    aegis_tool_call_destroy(c);
    printf("tool_call PASS\n");
}

static void test_tool_result(void)
{
    aegis_message_tool_result_t* r = NULL;
    expect_ok(aegis_message_tool_result_create(&r), "create result");
    expect_ok(aegis_message_tool_result_set_call_id(r, "call_123"), "set call_id");
    expect_ok(aegis_message_tool_result_set_content(r, "file content"), "set content");
    expect_ok(aegis_message_tool_result_set_status(r, AEGIS_OK), "set status");
    assert(strcmp(aegis_message_tool_result_call_id(r), "call_123") == 0);
    assert(strcmp(aegis_message_tool_result_content(r), "file content") == 0);
    aegis_message_tool_result_t* c = NULL;
    expect_ok(aegis_message_tool_result_clone(r, &c), "clone result");
    aegis_message_tool_result_destroy(c);
    aegis_message_tool_result_destroy(r);
    printf("tool_result PASS\n");
}

static void test_message_lifecycle(void)
{
    aegis_message_t* m = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "create user");
    expect_ok(aegis_message_set_content(m, "hello"), "set content");
    assert(strcmp(aegis_message_content(m), "hello") == 0);
    assert(aegis_message_role(m) == AEGIS_MESSAGE_USER);
    assert(aegis_message_id(m) != NULL);
    aegis_message_t* clone = NULL;
    expect_ok(aegis_message_clone(m, &clone), "clone");
    assert(strcmp(aegis_message_content(clone), "hello") == 0);
    aegis_message_destroy(clone);
    aegis_message_destroy(m);
    printf("message lifecycle PASS\n");
}

static void test_message_tool_calls(void)
{
    aegis_message_t* m = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &m), "create assistant");
    aegis_tool_call_t* c = NULL;
    expect_ok(aegis_tool_call_create(&c), "create call");
    expect_ok(aegis_tool_call_set_name(c, "edit"), "name");
    expect_ok(aegis_tool_call_set_id(c, "call_1"), "id");
    expect_ok(aegis_message_add_tool_call(m, c), "add");
    assert(aegis_message_tool_call_count(m) == 1);
    const aegis_tool_call_t* got = aegis_message_tool_call_at(m, 0);
    assert(got && strcmp(aegis_tool_call_name(got), "edit") == 0);
    aegis_tool_call_destroy(c);
    aegis_message_destroy(m);
    printf("message tool_calls PASS\n");
}

static void test_message_list(void)
{
    aegis_message_list_t* list = NULL;
    expect_ok(aegis_message_list_create(&list), "create list");
    assert(aegis_message_list_count(list) == 0);
    aegis_message_t* u = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &u), "create u");
    expect_ok(aegis_message_set_content(u, "hi"), "set hi");
    expect_ok(aegis_message_list_append(list, u), "append");
    assert(aegis_message_list_count(list) == 1);
    const aegis_message_t* at = aegis_message_list_at(list, 0);
    assert(at && strcmp(aegis_message_content(at), "hi") == 0);
    aegis_message_t* a = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &a), "create a");
    expect_ok(aegis_message_set_content(a, "hello"), "set hello");
    expect_ok(aegis_message_list_append(list, a), "append2");
    assert(aegis_message_list_count(list) == 2);
    aegis_message_t* s = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_SYSTEM, &s), "create s");
    expect_ok(aegis_message_set_content(s, "sys"), "set sys");
    expect_ok(aegis_message_list_prepend(list, s), "prepend");
    assert(aegis_message_list_count(list) == 3);
    assert(aegis_message_role(aegis_message_list_at(list, 0)) == AEGIS_MESSAGE_SYSTEM);
    aegis_message_list_t* clone = NULL;
    expect_ok(aegis_message_list_clone(list, &clone), "clone list");
    assert(aegis_message_list_count(clone) == 3);
    aegis_message_list_destroy(clone);
    aegis_message_destroy(s);
    aegis_message_destroy(a);
    aegis_message_destroy(u);
    aegis_message_list_destroy(list);
    printf("message_list PASS\n");
}

static void test_null_safety(void)
{
    aegis_message_destroy(NULL);
    aegis_message_list_destroy(NULL);
    aegis_tool_call_destroy(NULL);
    aegis_message_tool_result_destroy(NULL);
    assert(aegis_message_content(NULL) == NULL);
    assert(aegis_message_list_count(NULL) == 0);
    printf("null safety PASS\n");
}

int main(void)
{
    test_role_str();
    test_tool_call_lifecycle();
    test_tool_result();
    test_message_lifecycle();
    test_message_tool_calls();
    test_message_list();
    test_null_safety();
    printf("All message tests PASS\n");
    return 0;
}
