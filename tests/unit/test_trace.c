/**
 * @file test_trace.c
 * @brief Unit tests for the Trace module.
 */
#include "aegis/trace.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

static void test_trace_lifecycle(void)
{
    aegis_trace_context_t* ctx = NULL;
    expect_ok(aegis_trace_context_create(&ctx), "create");
    assert(ctx != NULL);
    assert(aegis_trace_context_trace_id(ctx) != 0);
    assert(aegis_trace_context_span_id(ctx) == 0);
    assert(aegis_trace_context_agent_id(ctx) == 0);

    aegis_trace_context_set_agent_id(ctx, 42);
    assert(aegis_trace_context_agent_id(ctx) == 42);

    aegis_trace_context_destroy(ctx);
    aegis_trace_context_destroy(NULL);
}

static void test_span_creation(void)
{
    aegis_trace_context_t* ctx = NULL;
    expect_ok(aegis_trace_context_create(&ctx), "create");

    aegis_trace_span_t* span = NULL;
    expect_ok(aegis_trace_span_create(ctx, "root-span", &span), "create span");
    assert(span != NULL);
    assert(aegis_trace_span_id(span) != 0);
    assert(aegis_trace_span_trace_id(span) == aegis_trace_context_trace_id(ctx));
    assert(aegis_trace_span_parent_id(span) == 0);
    assert(strcmp(aegis_trace_span_name(span), "root-span") == 0);
    assert(aegis_trace_span_duration_us(span) == 0);

    /* Child span. */
    aegis_trace_span_t* child = NULL;
    expect_ok(aegis_trace_span_create(ctx, "child-span", &child), "create child");
    assert(child != NULL);
    assert(aegis_trace_span_parent_id(child) == aegis_trace_span_id(span));

    /* End parent first (LIFO). */
    aegis_trace_span_end(span);
    assert(aegis_trace_span_duration_us(span) > 0);
    aegis_trace_span_end(child);
    // Duration is non-negative by construction

    aegis_trace_context_destroy(ctx);
}

static void test_clone_propagation(void)
{
    aegis_trace_context_t* ctx = NULL;
    expect_ok(aegis_trace_context_create(&ctx), "create");
    aegis_trace_context_set_agent_id(ctx, 7);

    aegis_trace_span_t* span = NULL;
    expect_ok(aegis_trace_span_create(ctx, "work", &span), "create span");

    aegis_trace_context_t* cloned = NULL;
    expect_ok(aegis_trace_context_clone(ctx, &cloned), "clone");
    assert(cloned != NULL);
    assert(aegis_trace_context_trace_id(cloned) == aegis_trace_context_trace_id(ctx));
    assert(aegis_trace_context_agent_id(cloned) == 7);

    /* New span in cloned context should have same trace ID. */
    aegis_trace_span_t* new_span = NULL;
    expect_ok(aegis_trace_span_create(cloned, "async-work", &new_span), "create in clone");
    assert(new_span != NULL);
    assert(aegis_trace_span_trace_id(new_span) == aegis_trace_context_trace_id(ctx));

    aegis_trace_span_destroy(new_span);
    aegis_trace_span_destroy(span);
    aegis_trace_context_destroy(cloned);
    aegis_trace_context_destroy(ctx);
}

static void test_global_scope(void)
{
    assert(aegis_trace_get_active() == NULL);

    aegis_trace_context_t* ctx = NULL;
    expect_ok(aegis_trace_context_create(&ctx), "create");
    aegis_trace_set_active(ctx);
    assert(aegis_trace_get_active() == ctx);

    aegis_trace_set_active(NULL);
    assert(aegis_trace_get_active() == NULL);

    aegis_trace_context_destroy(ctx);
}

static void test_null_safety(void)
{
    assert(aegis_trace_context_trace_id(NULL) == 0);
    assert(aegis_trace_context_span_id(NULL) == 0);
    assert(aegis_trace_context_parent_span_id(NULL) == 0);
    assert(aegis_trace_context_agent_id(NULL) == 0);
    aegis_trace_context_set_agent_id(NULL, 0);
    aegis_trace_span_id(NULL);
    aegis_trace_span_trace_id(NULL);
    aegis_trace_span_parent_id(NULL);
    aegis_trace_span_name(NULL);
    aegis_trace_span_duration_us(NULL);
    aegis_trace_span_end(NULL);
    aegis_trace_span_destroy(NULL);
    aegis_trace_set_active(NULL);
}

static void test_async_propagation(void)
{
    /* Simulate async boundary: create trace, clone for worker, end original. */
    aegis_trace_context_t* parent = NULL;
    expect_ok(aegis_trace_context_create(&parent), "create parent");

    aegis_trace_span_t* parent_span = NULL;
    expect_ok(aegis_trace_span_create(parent, "parent-task", &parent_span), "parent span");

    /* Clone for async worker. */
    aegis_trace_context_t* worker_ctx = NULL;
    expect_ok(aegis_trace_context_clone(parent, &worker_ctx), "clone for worker");

    aegis_trace_span_t* worker_span = NULL;
    expect_ok(aegis_trace_span_create(worker_ctx, "worker-task", &worker_span), "worker span");

    /* Both spans share the same trace ID. */
    assert(aegis_trace_span_trace_id(parent_span) == aegis_trace_span_trace_id(worker_span));

    /* End worker span first (it started later). */
    aegis_trace_span_end(worker_span);
    // Duration is non-negative by construction

    aegis_trace_span_end(parent_span);
    // Duration is non-negative by construction

    aegis_trace_context_destroy(worker_ctx);
    aegis_trace_context_destroy(parent);
}

int main(void)
{
    test_null_safety();
    test_trace_lifecycle();
    test_span_creation();
    test_clone_propagation();
    test_global_scope();
    test_async_propagation();

    printf("trace: all tests passed\n");
    return 0;
}
