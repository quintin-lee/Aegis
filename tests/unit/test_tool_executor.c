/**
 * @file test_tool_executor.c
 * @brief End-to-end tests: Task -> Executor -> Tool Registry -> Tool ->
 *        Result, using mock tools registered in a real registry and
 *        dispatched through aegis_tool_submit().
 */
#include "aegis/tool/tool.h"
#include "aegis/common/time.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Mock tools ─────────────────────────────────────────────────────── */

static const aegis_tool_param_spec_t k_add_params[] = {
    {"a", AEGIS_TOOL_VAL_INT, true, NULL},
    {"b", AEGIS_TOOL_VAL_INT, true, NULL},
};
static const aegis_tool_schema_t k_add_schema = {k_add_params, 2};

static aegis_status_t add_ints(void* user, const aegis_tool_args_t* args,
                               const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t *a = NULL, *b = NULL;
    if (!aegis_tool_args_find(args, "a", &a) || !aegis_tool_args_find(args, "b", &b)) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_tool_result_set_int(out, a->as.i + b->as.i);
}

static const aegis_tool_param_spec_t k_echo_params[] = {
    {"text", AEGIS_TOOL_VAL_STRING, true, NULL},
};
static const aegis_tool_schema_t k_echo_schema = {k_echo_params, 1};

/* Schema-less tools (failing/sleeper/cancelme accept no arguments). */
static const aegis_tool_schema_t k_no_schema = {NULL, 0};

static aegis_status_t echo_string(void* user, const aegis_tool_args_t* args,
                                  const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t* t = NULL;
    if (!aegis_tool_args_find(args, "text", &t)) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_tool_result_set_string(out, t->as.str.ptr);
}

static aegis_status_t failing_tool(void* user, const aegis_tool_args_t* args,
                                   const aegis_cancellation_token_t* token,
                                   aegis_tool_result_t*              out)
{
    (void)args;
    (void)token;
    (void)out;
    return (aegis_status_t)(intptr_t)user; /* verbatim error code */
}

/* Sleeps in 5ms slices honoring the token; ~200ms max. */
static aegis_status_t sleeper(void* user, const aegis_tool_args_t* args,
                              const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)args;
    (void)out;
    for (int i = 0; i < 40; i++) {
        if (aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        aegis_sleep_ms(5);
    }
    return aegis_tool_result_set_bool(out, true);
}

/* Spins until the token is tripped, then reports cooperative cancel. */
static aegis_status_t cancelme(void* user, const aegis_tool_args_t* args,
                               const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)args;
    (void)out;
    while (!aegis_cancellation_token_is_cancelled(token)) {
        aegis_sleep_ms(1);
    }
    return AEGIS_ERR_CANCELLED;
}

/* ── Fixtures ───────────────────────────────────────────────────────── */

static aegis_task_t* make_task(const char* name)
{
    aegis_task_t* t = NULL;
    assert(aegis_task_create(&t, name, "") == AEGIS_OK);
    return t;
}

static aegis_executor_t* make_executor(unsigned workers)
{
    aegis_executor_config_t cfg;
    aegis_executor_t*       exec = NULL;
    cfg.worker_count             = workers;
    cfg.queue_capacity           = 64;
    assert(aegis_executor_create(&exec, &cfg) == AEGIS_OK);
    return exec;
}

#define SPIN_COND(cond, label)                                            \
    do {                                                                  \
        for (int spin_##label = 0; spin_##label < 5000; spin_##label++) { \
            if (cond)                                                     \
                break;                                                    \
            aegis_sleep_ms(1);                                            \
        }                                                                 \
        assert(cond); /* label: bounded-wait exceeded */                  \
    } while (0)

static aegis_tool_registry_t* make_registry_with_mocks(void)
{
    aegis_tool_registry_t* reg = NULL;
    assert(aegis_tool_registry_create(&reg) == AEGIS_OK);

    aegis_tool_def_t d;
    memset(&d, 0, sizeof(d));

    d.name        = "add_ints";
    d.description = "sums two ints";
    d.schema      = k_add_schema;
    d.execute     = add_ints;
    assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);

    d.name        = "echo_string";
    d.description = "echoes its text argument";
    d.schema      = k_echo_schema;
    d.execute     = echo_string;
    assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);

    d.name        = "failing";
    d.description = "always fails with ERR_PROVIDER";
    d.schema      = k_no_schema;
    d.execute     = failing_tool;
    d.user        = (void*)(intptr_t)AEGIS_ERR_PROVIDER;
    assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);

    d.name        = "sleeper";
    d.description = "sleeps honoring the token";
    d.schema      = k_no_schema;
    d.execute     = sleeper;
    d.user        = NULL;
    assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);

    d.name        = "cancelme";
    d.description = "spins until cancelled";
    d.schema      = k_no_schema;
    d.execute     = cancelme;
    assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);

    return reg;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static aegis_tool_args_t* fresh_args(void)
{
    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    return args;
}

static void test_submit_validation(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("v");

    /* A failed submit consumes (destroys) the transferred args, so each
     * assertion gets its own list. */
    assert(aegis_tool_submit(NULL, reg, task, "add_ints", fresh_args()) == AEGIS_ERR_INVALID);
    assert(aegis_tool_submit(exec, NULL, task, "add_ints", fresh_args()) == AEGIS_ERR_INVALID);
    assert(aegis_tool_submit(exec, reg, NULL, "add_ints", fresh_args()) == AEGIS_ERR_INVALID);
    assert(aegis_tool_submit(exec, reg, task, NULL, fresh_args()) == AEGIS_ERR_INVALID);

    /* NULL args are rejected before transfer: nothing to clean up. */
    assert(aegis_tool_submit(exec, reg, task, "add_ints", NULL) == AEGIS_ERR_INVALID);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_unknown_tool_propagates_not_found(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("unknown");

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);

    /* Submit succeeds: resolution happens at execution time. */
    assert(aegis_tool_submit(exec, reg, task, "no_such_tool", args) == AEGIS_OK);

    aegis_exec_result_t result;
    memset(&result, 0, sizeof(result));
    assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
    assert(result.outcome == AEGIS_EXEC_FAILED);
    assert(result.status == AEGIS_ERR_NOT_FOUND);
    assert(result.attempts == 1);
    assert(aegis_task_state(task) == AEGIS_TASK_FAILED);
    const char* err = aegis_task_error(task);
    assert(err && strstr(err, "no_such_tool") != NULL);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_success_paths_encode_output_bytes(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(2);

    /* INT result -> 8 bytes little-endian. */
    {
        aegis_task_t*      task = make_task("add");
        aegis_tool_args_t* args = NULL;
        assert(aegis_tool_args_create(&args) == AEGIS_OK);
        assert(aegis_tool_args_add_int(args, "a", 19) == AEGIS_OK);
        assert(aegis_tool_args_add_int(args, "b", -7) == AEGIS_OK);
        assert(aegis_tool_submit(exec, reg, task, "add_ints", args) == AEGIS_OK);

        aegis_exec_result_t result;
        memset(&result, 0, sizeof(result));
        assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
        assert(result.outcome == AEGIS_EXEC_COMPLETED);
        assert(result.status == AEGIS_OK);
        assert(aegis_task_state(task) == AEGIS_TASK_SUCCESS);

        size_t      len  = 0;
        const void* data = aegis_task_output(task, &len);
        assert(data && len == 8);
        uint8_t expect[8];
        int64_t sum = 12;
        memcpy(expect, &sum, 8); /* little-endian host */
        assert(memcmp(data, expect, 8) == 0);

        aegis_task_destroy(task);
    }

    /* STRING result -> raw text without NUL. */
    {
        aegis_task_t*      task = make_task("echo");
        aegis_tool_args_t* args = NULL;
        assert(aegis_tool_args_create(&args) == AEGIS_OK);
        assert(aegis_tool_args_add_string(args, "text", "hello-tool") == AEGIS_OK);
        assert(aegis_tool_submit(exec, reg, task, "echo_string", args) == AEGIS_OK);

        aegis_exec_result_t result;
        memset(&result, 0, sizeof(result));
        assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
        assert(result.outcome == AEGIS_EXEC_COMPLETED);

        size_t      len  = 0;
        const void* data = aegis_task_output(task, &len);
        assert(data && len == 10);
        assert(memcmp(data, "hello-tool", 10) == 0);

        aegis_task_destroy(task);
    }

    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_validation_failure_propagates_invalid(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("badargs");

    /* add_ints requires INT a,b; supply wrong-typed + missing instead. */
    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_float(args, "a", 1.5) == AEGIS_OK);
    assert(aegis_tool_submit(exec, reg, task, "add_ints", args) == AEGIS_OK);

    aegis_exec_result_t result;
    memset(&result, 0, sizeof(result));
    assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
    assert(result.outcome == AEGIS_EXEC_FAILED);
    assert(result.status == AEGIS_ERR_INVALID); /* validation, verbatim */
    assert(result.attempts == 1);
    assert(aegis_task_state(task) == AEGIS_TASK_FAILED);
    const char* err = aegis_task_error(task);
    assert(err && strstr(err, "add_ints") != NULL);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_tool_error_propagates_verbatim(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("boom");

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_submit(exec, reg, task, "failing", args) == AEGIS_OK);

    aegis_exec_result_t result;
    memset(&result, 0, sizeof(result));
    assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
    assert(result.outcome == AEGIS_EXEC_FAILED);
    assert(result.status == AEGIS_ERR_PROVIDER); /* verbatim from tool */
    assert(aegis_task_state(task) == AEGIS_TASK_FAILED);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_timeout_via_task_deadline(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("slow");

    aegis_task_set_timeout_ms(task, 50); /* sleeper polls every 5ms */

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_submit(exec, reg, task, "sleeper", args) == AEGIS_OK);

    aegis_exec_result_t result;
    memset(&result, 0, sizeof(result));
    assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
    assert(result.outcome == AEGIS_EXEC_TIMED_OUT);
    assert(result.status == AEGIS_ERR_TIMEOUT);
    assert(result.attempts == 1); /* timeout never retried */
    assert(aegis_task_state(task) == AEGIS_TASK_FAILED);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_cooperative_cancel_running_tool(void)
{
    aegis_tool_registry_t* reg  = make_registry_with_mocks();
    aegis_executor_t*      exec = make_executor(1);
    aegis_task_t*          task = make_task("victim");

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_submit(exec, reg, task, "cancelme", args) == AEGIS_OK);

    SPIN_COND(aegis_executor_running_count(exec) == 1, running);

    assert(aegis_executor_cancel(exec, aegis_task_id(task)) == AEGIS_OK);

    aegis_exec_result_t result;
    memset(&result, 0, sizeof(result));
    assert(aegis_executor_wait(exec, aegis_task_id(task), &result, -1) == AEGIS_OK);
    assert(result.outcome == AEGIS_EXEC_CANCELLED);
    assert(result.status == AEGIS_ERR_CANCELLED);
    assert(aegis_task_state(task) == AEGIS_TASK_CANCELLED);

    aegis_task_destroy(task);
    aegis_executor_destroy(exec);
    aegis_tool_registry_destroy(reg);
}

static void test_direct_call_without_executor(void)
{
    aegis_tool_registry_t* reg = make_registry_with_mocks();

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_int(args, "a", 40) == AEGIS_OK);
    assert(aegis_tool_args_add_int(args, "b", 2) == AEGIS_OK);

    aegis_tool_result_t out;
    memset(&out, 0, sizeof(out));
    assert(aegis_tool_call(reg, "add_ints", args, 1000, &out) == AEGIS_OK);
    assert(out.value.type == AEGIS_TOOL_VAL_INT && out.value.as.i == 42);
    aegis_tool_result_destroy(&out);

    /* Validation failure surfaces directly. */
    memset(&out, 0, sizeof(out));
    assert(aegis_tool_call(reg, "add_ints", NULL, 1000, &out) == AEGIS_ERR_INVALID);
    assert(aegis_tool_call(reg, "ghost", args, 1000, &out) == AEGIS_ERR_NOT_FOUND);

    aegis_tool_args_destroy(args);
    aegis_tool_registry_destroy(reg);
}

int main(void)
{
    test_submit_validation();
    test_unknown_tool_propagates_not_found();
    test_success_paths_encode_output_bytes();
    test_validation_failure_propagates_invalid();
    test_tool_error_propagates_verbatim();
    test_timeout_via_task_deadline();
    test_cooperative_cancel_running_tool();
    test_direct_call_without_executor();
    printf("test_tool_executor: all cases passed\n");
    return 0;
}
