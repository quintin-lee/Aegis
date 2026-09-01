#define _POSIX_C_SOURCE 200809L
#include "aegis/session/session.h"
#include "aegis/message/message.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc);
        assert(0);
    }
}

static void test_create_append(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/proj", &s), "create");
    assert(aegis_session_id(s) != NULL);
    assert(aegis_session_message_count(s) == 0);
    aegis_message_t* m = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "msg create");
    expect_ok(aegis_message_set_content(m, "hello"), "set");
    expect_ok(aegis_session_append_message(s, m), "append");
    assert(aegis_session_message_count(s) == 1);
    const aegis_message_t* at = aegis_session_message_at(s, 0);
    assert(at && strcmp(aegis_message_content(at), "hello") == 0);
    aegis_message_destroy(m);
    aegis_session_destroy(s);
    printf("create_append PASS\n");
}

static void test_save_load(void)
{
    char tmpl[] = "/tmp/aegis_sess_XXXXXX";
    char* tmp = mkdtemp(tmpl);
    assert(tmp);
    char path[1024];
    snprintf(path, sizeof(path), "%s/sess.jsonl", tmp);
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/proj2", &s), "create2");
    aegis_message_t* m1 = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m1), "m1");
    expect_ok(aegis_message_set_content(m1, "user hello"), "c1");
    expect_ok(aegis_session_append_message(s, m1), "append1");
    aegis_message_t* m2 = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &m2), "m2");
    expect_ok(aegis_message_set_content(m2, "assistant hi"), "c2");
    expect_ok(aegis_session_append_message(s, m2), "append2");
    expect_ok(aegis_session_save(s, path), "save");
    aegis_session_t* loaded = NULL;
    expect_ok(aegis_session_load(path, &loaded), "load");
    assert(aegis_session_message_count(loaded) == 2);
    assert(strcmp(aegis_message_content(aegis_session_message_at(loaded, 0)), "user hello") == 0);
    assert(strcmp(aegis_message_content(aegis_session_message_at(loaded, 1)), "assistant hi") == 0);
    aegis_message_destroy(m1);
    aegis_message_destroy(m2);
    aegis_session_destroy(s);
    aegis_session_destroy(loaded);
    unlink(path);
    rmdir(tmp);
    printf("save_load PASS\n");
}

static void test_tool_call_round_trip(void)
{
    char tmpl[] = "/tmp/aegis_tool_sess_XXXXXX";
    char* tmp = mkdtemp(tmpl);
    assert(tmp);
    char path[1024];
    snprintf(path, sizeof(path), "%s/sess.jsonl", tmp);

    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/tool", &s), "tool session");
    aegis_message_t* assistant = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &assistant), "assistant");
    aegis_tool_call_t* call = NULL;
    expect_ok(aegis_tool_call_create(&call), "call");
    expect_ok(aegis_tool_call_set_id(call, "call-42"), "call id");
    expect_ok(aegis_tool_call_set_name(call, "read"), "call name");
    expect_ok(aegis_tool_call_set_arguments(call, "{\"path\":\"README.md\",\"note\":\"a\\n\\\"quote\\\"\"}"), "call args");
    expect_ok(aegis_tool_call_set_index(call, 3), "call index");
    expect_ok(aegis_message_add_tool_call(assistant, call), "attach call");
    expect_ok(aegis_session_append_message(s, assistant), "append assistant");
    expect_ok(aegis_session_save(s, path), "save tool session");

    aegis_session_t* loaded = NULL;
    expect_ok(aegis_session_load(path, &loaded), "load tool session");
    assert(aegis_session_message_count(loaded) == 1);
    const aegis_message_t* restored = aegis_session_message_at(loaded, 0);
    assert(aegis_message_tool_call_count(restored) == 1);
    const aegis_tool_call_t* restored_call = aegis_message_tool_call_at(restored, 0);
    assert(strcmp(aegis_tool_call_id(restored_call), "call-42") == 0);
    assert(strcmp(aegis_tool_call_name(restored_call), "read") == 0);
    assert(strcmp(aegis_tool_call_arguments(restored_call), "{\"path\":\"README.md\",\"note\":\"a\\n\\\"quote\\\"\"}") == 0);
    assert(aegis_tool_call_index(restored_call) == 3);

    aegis_tool_call_destroy(call);
    aegis_message_destroy(assistant);
    aegis_session_destroy(s);
    aegis_session_destroy(loaded);
    unlink(path);
    rmdir(tmp);
    printf("tool_call_round_trip PASS\n");
}

static void test_compact(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/compact", &s), "compact session");
    for (int i = 0; i < 3; ++i) {
        aegis_message_t* m = NULL;
        expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "compact message");
        char content[32]; snprintf(content, sizeof(content), "message-%d", i);
        expect_ok(aegis_message_set_content(m, content), "compact content");
        expect_ok(aegis_session_append_message(s, m), "compact append");
        aegis_message_destroy(m);
    }
    expect_ok(aegis_session_compact(s, 2), "compact");
    assert(aegis_session_message_count(s) == 2);
    assert(strcmp(aegis_message_content(aegis_session_message_at(s, 0)), "message-1") == 0);
    assert(strcmp(aegis_message_content(aegis_session_message_at(s, 1)), "message-2") == 0);
    aegis_session_destroy(s);
    printf("compact PASS\\n");
}

static void test_fork(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/proj", &s), "create");
    aegis_message_t* m = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "msg");
    expect_ok(aegis_message_set_content(m, "root msg"), "content");
    expect_ok(aegis_session_append_message(s, m), "append");
    aegis_session_t* forked = NULL;
    expect_ok(aegis_session_fork(s, &forked), "fork");
    assert(aegis_session_message_count(forked) == 1);
    assert(strcmp(aegis_session_branch_id(s), aegis_session_branch_id(forked)) != 0);
    assert(strcmp(aegis_session_parent_id(forked), aegis_session_id(s)) == 0);
    aegis_message_t* m2 = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m2), "m2");
    expect_ok(aegis_message_set_content(m2, "fork msg"), "c2");
    expect_ok(aegis_session_append_message(forked, m2), "append fork");
    assert(aegis_session_message_count(s) == 1);
    assert(aegis_session_message_count(forked) == 2);
    aegis_message_destroy(m);
    aegis_message_destroy(m2);
    aegis_session_destroy(s);
    aegis_session_destroy(forked);
    printf("fork PASS\n");
}

int main(void)
{
    test_create_append();
    test_save_load();
    test_tool_call_round_trip();
    test_compact();
    test_fork();
    printf("All session tests PASS\n");
    return 0;
}
