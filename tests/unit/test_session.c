#define _POSIX_C_SOURCE 200809L
/**
 * @file test_session.c
 * @brief Unit tests for session lifecycle, persistence, fork and compact.
 */
#include "aegis/session/session.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/common/cancellation/cancellation.h"
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
    char  tmpl[] = "/tmp/aegis_sess_XXXXXX";
    char* tmp    = mkdtemp(tmpl);
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

static void test_reasoning_round_trip(void)
{
    char  tmpl[] = "/tmp/aegis_reason_sess_XXXXXX";
    char* tmp    = mkdtemp(tmpl);
    assert(tmp);
    char path[1024];
    snprintf(path, sizeof(path), "%s/sess.jsonl", tmp);

    /* With reasoning: must round-trip intact. */
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/proj3", &s), "create r");
    aegis_message_t* m = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &m), "m");
    expect_ok(aegis_message_set_content(m, "answer"), "c");
    expect_ok(aegis_message_set_reasoning(m, "deep \"thoughts\"\nline2"), "r");
    expect_ok(aegis_session_append_message(s, m), "append r");
    aegis_message_destroy(m);
    expect_ok(aegis_session_save(s, path), "save r");
    aegis_session_t* loaded = NULL;
    expect_ok(aegis_session_load(path, &loaded), "load r");
    assert(aegis_session_message_count(loaded) == 1);
    const aegis_message_t* rm = aegis_session_message_at(loaded, 0);
    assert(rm && aegis_message_reasoning(rm) != NULL);
    assert(strcmp(aegis_message_reasoning(rm), "deep \"thoughts\"\nline2") == 0);
    aegis_session_destroy(s);
    aegis_session_destroy(loaded);
    unlink(path);

    /* Without reasoning: must load with NULL reasoning (backward compat). */
    expect_ok(aegis_session_create("/tmp/proj3", &s), "create n");
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &m), "m n");
    expect_ok(aegis_message_set_content(m, "plain"), "c n");
    expect_ok(aegis_session_append_message(s, m), "append n");
    aegis_message_destroy(m);
    expect_ok(aegis_session_save(s, path), "save n");
    expect_ok(aegis_session_load(path, &loaded), "load n");
    const aegis_message_t* nm = aegis_session_message_at(loaded, 0);
    assert(nm && aegis_message_reasoning(nm) == NULL);
    aegis_session_destroy(s);
    aegis_session_destroy(loaded);
    unlink(path);
    rmdir(tmp);
    printf("reasoning_round_trip PASS\n");
}

static void test_tool_call_round_trip(void)
{
    char  tmpl[] = "/tmp/aegis_tool_sess_XXXXXX";
    char* tmp    = mkdtemp(tmpl);
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
    expect_ok(aegis_tool_call_set_arguments(
                  call, "{\"path\":\"README.md\",\"note\":\"a\\n\\\"quote\\\"\"}"),
              "call args");
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
    assert(strcmp(aegis_tool_call_arguments(restored_call),
                  "{\"path\":\"README.md\",\"note\":\"a\\n\\\"quote\\\"\"}") == 0);
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
        char content[32];
        snprintf(content, sizeof(content), "message-%d", i);
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

static void test_compact_preserves_tool_group(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/compact-tools", &s), "tool compact session");
    aegis_message_t* user = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &user), "user");
    expect_ok(aegis_message_set_content(user, "request"), "user content");
    expect_ok(aegis_session_append_message(s, user), "append user");
    aegis_message_t* assistant = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &assistant), "assistant");
    aegis_tool_call_t* call = NULL;
    expect_ok(aegis_tool_call_create(&call), "compact call");
    expect_ok(aegis_tool_call_set_id(call, "compact-call"), "compact call id");
    expect_ok(aegis_tool_call_set_name(call, "read"), "compact call name");
    expect_ok(aegis_tool_call_set_arguments(call, "{}"), "compact call args");
    expect_ok(aegis_message_add_tool_call(assistant, call), "attach compact call");
    expect_ok(aegis_session_append_message(s, assistant), "append assistant");
    aegis_message_t* tool = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_TOOL, &tool), "tool");
    expect_ok(aegis_message_set_tool_call_id(tool, "compact-call"), "tool id");
    expect_ok(aegis_message_set_content(tool, "result"), "tool content");
    expect_ok(aegis_session_append_message(s, tool), "append tool");
    expect_ok(aegis_session_compact(s, 1), "compact tool group");
    assert(aegis_session_message_count(s) == 2);
    assert(aegis_message_role(aegis_session_message_at(s, 0)) == AEGIS_MESSAGE_ASSISTANT);
    assert(aegis_message_tool_call_count(aegis_session_message_at(s, 0)) == 1);
    assert(aegis_message_role(aegis_session_message_at(s, 1)) == AEGIS_MESSAGE_TOOL);
    aegis_tool_call_destroy(call);
    aegis_message_destroy(user);
    aegis_message_destroy(assistant);
    aegis_message_destroy(tool);
    aegis_session_destroy(s);
    printf("compact_tool_group PASS\\n");
}

static void test_compact_preserves_multiple_tool_results(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/compact-multi", &s), "multi compact session");
    aegis_message_t* assistant = NULL;
    expect_ok(aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &assistant), "multi assistant");
    const char* ids[] = {"call-a", "call-b"};
    for (size_t i = 0; i < 2; ++i) {
        aegis_tool_call_t* c = NULL;
        expect_ok(aegis_tool_call_create(&c), "multi call");
        expect_ok(aegis_tool_call_set_id(c, ids[i]), "multi id");
        expect_ok(aegis_tool_call_set_name(c, "read"), "multi name");
        expect_ok(aegis_tool_call_set_arguments(c, "{}"), "multi args");
        expect_ok(aegis_message_add_tool_call(assistant, c), "multi attach");
        aegis_tool_call_destroy(c);
    }
    expect_ok(aegis_session_append_message(s, assistant), "multi append assistant");
    for (size_t i = 0; i < 2; ++i) {
        aegis_message_t* tool = NULL;
        expect_ok(aegis_message_create(AEGIS_MESSAGE_TOOL, &tool), "multi tool");
        expect_ok(aegis_message_set_tool_call_id(tool, ids[i]), "multi tool id");
        expect_ok(aegis_message_set_content(tool, "result"), "multi result");
        expect_ok(aegis_session_append_message(s, tool), "multi append tool");
        aegis_message_destroy(tool);
    }
    expect_ok(aegis_session_compact(s, 1), "compact multiple results");
    assert(aegis_session_message_count(s) == 3);
    assert(aegis_message_tool_call_count(aegis_session_message_at(s, 0)) == 2);
    assert(strcmp(aegis_message_tool_call_id(aegis_session_message_at(s, 1)), "call-a") == 0);
    assert(strcmp(aegis_message_tool_call_id(aegis_session_message_at(s, 2)), "call-b") == 0);
    aegis_message_destroy(assistant);
    aegis_session_destroy(s);
    printf("compact_multiple_tool_results PASS\\n");
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

static aegis_status_t failing_complete(void* user, const aegis_model_request_t* req,
                                       const aegis_cancellation_token_t* token,
                                       aegis_model_response_t**          out)
{
    (void)user;
    (void)req;
    (void)token;
    *out = NULL;
    return AEGIS_ERR_PROVIDER;
}

static aegis_session_t* make_session_with_user_msgs(const char* root, size_t total)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create(root, &s), "make session");
    for (size_t i = 0; i < total; ++i) {
        aegis_message_t* m = NULL;
        expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "make msg");
        char buf[32];
        snprintf(buf, sizeof(buf), "msg %zu", i);
        expect_ok(aegis_message_set_content(m, buf), "make msg content");
        expect_ok(aegis_session_append_message(s, m), "make msg append");
        aegis_message_destroy(m);
    }
    return s;
}

static void test_compact_with_summary_null_model(void)
{
    aegis_session_t* s       = make_session_with_user_msgs("/tmp/sum-null", 10);
    aegis_message_t* summary = NULL;
    aegis_status_t   st =
        aegis_session_compact_with_summary(s, 3, /*model*/ NULL, /*token*/ NULL, &summary);
    assert(st == AEGIS_OK);
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 3);
    aegis_session_destroy(s);
    printf("compact_with_summary_null_model PASS\n");
}

static void test_compact_with_summary_mock_model(void)
{
    aegis_session_t*      s     = make_session_with_user_msgs("/tmp/sum-mock", 5);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "mock client");
    aegis_message_t* summary = NULL;
    aegis_status_t   st = aegis_session_compact_with_summary(s, 2, model, /*token*/ NULL, &summary);
    assert(st == AEGIS_OK);
    assert(summary != NULL);
    /* retained (2) + prepended summary (1) = 3. */
    assert(aegis_session_message_count(s) == 3);
    const aegis_message_t* head = aegis_session_message_at(s, 0);
    assert(aegis_message_role(head) == AEGIS_MESSAGE_ASSISTANT);
    /* Dropped [0,3) = msgs 0,1,2; last USER content 'msg 2' -> mock echoes it. */
    assert(strcmp(aegis_message_content(head), "mock response to: msg 2") == 0);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_with_summary_mock PASS\n");
}

static void test_compact_with_summary_model_fails(void)
{
    aegis_session_t*      s       = make_session_with_user_msgs("/tmp/sum-fail", 5);
    aegis_model_backend_t backend = {
        .user         = NULL,
        .complete     = failing_complete,
        .stream       = NULL,
        .capabilities = AEGIS_MODEL_CAP_TEXT,
    };
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create_with_backend("mock", &backend, &model), "fail client");
    aegis_message_t* summary = NULL;
    aegis_status_t   st = aegis_session_compact_with_summary(s, 2, model, /*token*/ NULL, &summary);
    assert(st == AEGIS_OK); /* truncation still happened */
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 2);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_with_summary_model_fails PASS\n");
}

static void test_compact_with_summary_cancelled_token(void)
{
    aegis_session_t*      s     = make_session_with_user_msgs("/tmp/sum-cancel", 5);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "cancel client");
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");
    aegis_cancellation_token_request_cancel(token);
    aegis_message_t* summary = NULL;
    aegis_status_t   st      = aegis_session_compact_with_summary(s, 2, model, token, &summary);
    assert(st == AEGIS_OK);
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 2);
    aegis_cancellation_token_destroy(token);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_with_summary_cancelled PASS\n");
}

static void test_compact_with_summary_noop(void)
{
    aegis_session_t*      s     = make_session_with_user_msgs("/tmp/sum-noop", 5);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "noop client");
    aegis_message_t* summary = NULL;
    aegis_status_t   st = aegis_session_compact_with_summary(s, 5, model, /*token*/ NULL, &summary);
    assert(st == AEGIS_OK);
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 5);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_with_summary_noop PASS\n");
}

int main(void)
{
    test_create_append();
    test_save_load();
    test_reasoning_round_trip();
    test_tool_call_round_trip();
    test_compact();
    test_compact_preserves_tool_group();
    test_compact_preserves_multiple_tool_results();
    test_fork();
    test_compact_with_summary_null_model();
    test_compact_with_summary_mock_model();
    test_compact_with_summary_model_fails();
    test_compact_with_summary_cancelled_token();
    test_compact_with_summary_noop();
    printf("All session tests PASS\n");
    return 0;
}
