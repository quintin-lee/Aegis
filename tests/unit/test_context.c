/**
 * @file test_context.c
 * @brief Unit tests for the Context module:
 *   - Builder lifecycle (create/destroy)
 *   - Section addition (valid/invalid args)
 *   - Priority-based sorting
 *   - Token budget truncation
 *   - Empty context
 *   - Compression callback
 *   - Cancellation
 *   - Accessors
 */
#include "aegis/context/context.h"
#include "aegis/message/message.h"
#include "aegis/common/cancellation/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* Mock compression: truncate to half the content. */
static size_t mock_compress(const char* content, size_t content_len, char* out_buf,
                            size_t out_buf_size)
{
    (void)content_len;
    size_t half = content_len / 2;
    if (half == 0) {
        half = 1;
    }
    if (half + 1 > out_buf_size) {
        half = out_buf_size - 1;
    }
    memcpy(out_buf, content, half);
    out_buf[half] = '\0';
    return half;
}

/* ── Builder lifecycle ─────────────────────────────────────────────────────── */

static void test_builder_lifecycle(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");
    assert(b != NULL);
    aegis_context_builder_destroy(b);
    aegis_context_builder_destroy(NULL); /* no-op */
}

/* ── Section addition ──────────────────────────────────────────────────────── */

static void test_add_section_valid(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    expect_ok(aegis_context_builder_add_section(b, "system prompt", AEGIS_CONTEXT_SYSTEM, 100, 0),
              "add system");
    expect_ok(aegis_context_builder_add_section(b, "goal text", AEGIS_CONTEXT_GOAL, 90, 0),
              "add goal");
    expect_ok(aegis_context_builder_add_section(b, "plan steps", AEGIS_CONTEXT_PLAN, 80, 0),
              "add plan");

    aegis_context_builder_destroy(b);
}

static void test_add_section_invalid(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    /* NULL content. */
    assert(aegis_context_builder_add_section(b, NULL, AEGIS_CONTEXT_SYSTEM, 50, 0) ==
           AEGIS_ERR_INVALID);

    /* Empty content. */
    assert(aegis_context_builder_add_section(b, "", AEGIS_CONTEXT_SYSTEM, 50, 0) ==
           AEGIS_ERR_INVALID);

    /* NULL builder. */
    assert(aegis_context_builder_add_section(NULL, "content", AEGIS_CONTEXT_SYSTEM, 50, 0) ==
           AEGIS_ERR_INVALID);

    aegis_context_builder_destroy(b);
}

/* ── Build empty context ──────────────────────────────────────────────────── */

static void test_build_empty(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build empty");
    assert(ctx != NULL);
    assert(strcmp(aegis_context_content(ctx), "") == 0);
    assert(aegis_context_token_estimate(ctx) == 0);
    assert(!aegis_context_is_truncated(ctx));

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

/* ── Priority sorting ──────────────────────────────────────────────────────── */

static void test_priority_sorting(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    /* Add sections in reverse priority order. */
    expect_ok(aegis_context_builder_add_section(b, "low", AEGIS_CONTEXT_HISTORY, 10, 0), "add low");
    expect_ok(aegis_context_builder_add_section(b, "high", AEGIS_CONTEXT_SYSTEM, 100, 0),
              "add high");
    expect_ok(aegis_context_builder_add_section(b, "medium", AEGIS_CONTEXT_GOAL, 50, 0),
              "add medium");

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build");
    assert(ctx != NULL);

    /* High priority should come first. */
    const char* content = aegis_context_content(ctx);
    assert(strstr(content, "high") != NULL);
    assert(strstr(content, "medium") != NULL);
    assert(strstr(content, "low") != NULL);
    /* Check order: high before medium before low. */
    assert(strstr(content, "high") < strstr(content, "medium"));
    assert(strstr(content, "medium") < strstr(content, "low"));

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

/* ── Token budget truncation ───────────────────────────────────────────────── */

static void test_budget_truncation(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    /* Add sections with explicit token estimates. */
    expect_ok(aegis_context_builder_add_section(b, "section A", AEGIS_CONTEXT_SYSTEM, 100, 5),
              "add A");
    expect_ok(aegis_context_builder_add_section(b, "section B", AEGIS_CONTEXT_GOAL, 90, 5),
              "add B");
    expect_ok(aegis_context_builder_add_section(b, "section C", AEGIS_CONTEXT_PLAN, 80, 5),
              "add C");

    /* Budget of 12 tokens should include A and B (10 total) but truncate C. */
    aegis_context_builder_set_budget(b, 12);

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build with budget");
    assert(ctx != NULL);
    assert(aegis_context_is_truncated(ctx));

    const char* content = aegis_context_content(ctx);
    assert(strstr(content, "section A") != NULL);
    assert(strstr(content, "section B") != NULL);
    assert(strstr(content, "section C") == NULL);

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

static void test_message_budget_truncation(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");
    expect_ok(aegis_context_builder_add_section(b, "system", AEGIS_CONTEXT_SYSTEM, 100, 5),
              "add system");
    expect_ok(aegis_context_builder_add_section(b, "recent", AEGIS_CONTEXT_HISTORY, 90, 5),
              "add recent");
    expect_ok(aegis_context_builder_add_section(b, "old", AEGIS_CONTEXT_HISTORY, 10, 5),
              "add old");
    aegis_context_builder_set_budget(b, 10);

    aegis_message_list_t* list = NULL;
    expect_ok(aegis_context_build_messages(b, NULL, &list), "build messages");
    assert(aegis_message_list_count(list) == 2);
    assert(strstr(aegis_message_content(aegis_message_list_at(list, 0)), "system") != NULL);
    assert(strstr(aegis_message_content(aegis_message_list_at(list, 1)), "recent") != NULL);
    aegis_message_list_destroy(list);
    aegis_context_builder_destroy(b);
}

static void test_budget_unlimited(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    expect_ok(aegis_context_builder_add_section(b, "content A", AEGIS_CONTEXT_SYSTEM, 100, 5),
              "add A");
    expect_ok(aegis_context_builder_add_section(b, "content B", AEGIS_CONTEXT_GOAL, 90, 5),
              "add B");

    /* No budget (0) means unlimited. */
    aegis_context_builder_set_budget(b, 0);

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build unlimited");
    assert(ctx != NULL);
    assert(!aegis_context_is_truncated(ctx));
    assert(strstr(aegis_context_content(ctx), "content A") != NULL);
    assert(strstr(aegis_context_content(ctx), "content B") != NULL);

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

/* ── Compression ───────────────────────────────────────────────────────────── */

static void test_compression(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    /* Add a large section that will be compressed. */
    char large[200];
    memset(large, 'X', sizeof(large));
    large[sizeof(large) - 1] = '\0';

    expect_ok(aegis_context_builder_add_section(b, large, AEGIS_CONTEXT_OBSERVATION, 50, 0),
              "add large");

    /* Enable compression with threshold of 100 chars. */
    aegis_context_builder_set_compression(b, mock_compress, NULL, 100);

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build with compression");
    assert(ctx != NULL);

    const char* content = aegis_context_content(ctx);
    /* Compressed content should be shorter than original. */
    assert(strlen(content) < sizeof(large));

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

/* ── Cancellation ──────────────────────────────────────────────────────────── */

static void test_cancellation(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "create token");
    aegis_cancellation_token_request_cancel(token);

    aegis_context_t* ctx = NULL;
    assert(aegis_context_build(b, token, &ctx) == AEGIS_ERR_CANCELLED);
    assert(ctx == NULL);

    aegis_cancellation_token_destroy(token);
    aegis_context_builder_destroy(b);
}

/* ── Accessors ─────────────────────────────────────────────────────────────── */

static void test_accessors(void)
{
    aegis_context_builder_t* b = NULL;
    expect_ok(aegis_context_builder_create(&b), "create");

    expect_ok(aegis_context_builder_add_section(b, "test content", AEGIS_CONTEXT_SYSTEM, 100, 0),
              "add content");

    aegis_context_t* ctx = NULL;
    expect_ok(aegis_context_build(b, NULL, &ctx), "build");
    assert(ctx != NULL);

    assert(aegis_context_token_estimate(ctx) > 0);
    assert(!aegis_context_is_truncated(ctx));
    assert(strcmp(aegis_context_content(ctx), "test content") == 0);

    aegis_context_destroy(ctx);
    aegis_context_builder_destroy(b);
}

/* ── Null safety ───────────────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    aegis_context_builder_destroy(NULL);
    aegis_context_destroy(NULL);

    assert(aegis_context_builder_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_context_build(NULL, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_context_token_estimate(NULL) == 0);
    assert(!aegis_context_is_truncated(NULL));
    assert(strcmp(aegis_context_content(NULL), "") == 0);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_null_safety();
    test_builder_lifecycle();
    test_add_section_valid();
    test_add_section_invalid();
    test_build_empty();
    test_priority_sorting();
    test_budget_truncation();
    test_message_budget_truncation();
    test_budget_unlimited();
    test_compression();
    test_cancellation();
    test_accessors();

    printf("context: all tests passed\n");
    return 0;
}
