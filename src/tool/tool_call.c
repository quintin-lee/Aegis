/**
 * @file tool_call.c
 * @brief Tool call runtime: resolve -> validate -> execute, plus the
 *        executor bridge (job + work function + submit).
 *
 * The bridge keeps the executor free of any concrete-tool dependency:
 * aegis_tool_submit() merely installs aegis_tool_work_fn as the task's
 * work function; the executor classifies outcomes exactly as it does for
 * any other work.
 */
#include "cancellation_internal.h"
#include "task_internal.h"
#include "aegis/executor/executor.h"
#include "tool_internal.h"

#include "aegis/common/time.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Core invocation ──────────────────────────────────────────────────── */

aegis_status_t aegis_tool_execute(aegis_tool_registry_t* reg, const char* name,
                                  const aegis_tool_args_t*          args,
                                  const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    if (!reg || !name || !out) {
        return AEGIS_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    aegis_tool_def_t def;
    aegis_status_t   st = aegis_tool_registry_find(reg, name, &def);
    if (st != AEGIS_OK) {
        return st;
    }

    st = aegis_tool_validate_args(&def.schema, args);
    if (st != AEGIS_OK) {
        return st;
    }

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    st = def.execute(def.user, args, token, out);
    if (st != AEGIS_OK) {
        /* Tool contract says failure leaves the result zeroed; enforce it
         * so no payload allocated by a misbehaving tool can leak. */
        aegis_tool_result_destroy(out);
        memset(out, 0, sizeof(*out));
    }
    return st;
}

aegis_status_t aegis_tool_call(aegis_tool_registry_t* reg, const char* name,
                               const aegis_tool_args_t* args, long timeout_ms,
                               aegis_tool_result_t* out)
{
    if (!out || timeout_ms == 0) {
        /* timeout_ms == 0 would mean an always-expired deadline; reject
         * rather than surprise callers. */
        return AEGIS_ERR_INVALID;
    }

    struct aegis_cancellation_token tok = {AEGIS_CANCEL_NONE, 0};
    if (timeout_ms > 0) {
        const int64_t now = (int64_t)aegis_mono_now();
        const int64_t ns  = (int64_t)timeout_ms * 1000000;
        tok.deadline_ns   = (ns > INT64_MAX - now) ? INT64_MAX : now + ns;
    }

    return aegis_tool_execute(reg, name, args, &tok, out);
}

/* ── Job lifecycle ────────────────────────────────────────────────────── */

aegis_status_t aegis_tool_job_create(aegis_tool_job_t** out, aegis_tool_registry_t* reg,
                                     const char* tool_name, aegis_tool_args_t* args)
{
    if (!out || !reg || !tool_name || tool_name[0] == '\0' || !args) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_tool_job_t* job = calloc(1, sizeof(*job));
    if (!job) {
        return AEGIS_ERR_NOMEM;
    }
    job->reg  = reg;
    job->args = args; /* Transferred. */

    const size_t n = strlen(tool_name) + 1;
    job->name      = malloc(n);
    if (!job->name) {
        aegis_tool_job_destroy(job);
        return AEGIS_ERR_NOMEM;
    }
    memcpy(job->name, tool_name, n);

    *out = job;
    return AEGIS_OK;
}

void aegis_tool_job_destroy(aegis_tool_job_t* job)
{
    if (!job) {
        return;
    }
    free(job->name);
    aegis_tool_args_destroy(job->args);
    free(job);
}

/* ── Result -> task-output byte encoding ──────────────────────────────── */

static void store_le64(uint8_t* dst, uint64_t v)
{
    for (size_t i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}

static aegis_status_t encode_value(const aegis_tool_value_t* v, aegis_task_t* task)
{
    switch (v->type) {
    case AEGIS_TOOL_VAL_BOOL: {
        const uint8_t b = v->as.b ? 1u : 0u;
        return aegis_task_set_output(task, &b, 1);
    }
    case AEGIS_TOOL_VAL_INT: {
        uint8_t buf[8];
        store_le64(buf, (uint64_t)v->as.i);
        return aegis_task_set_output(task, buf, sizeof(buf));
    }
    case AEGIS_TOOL_VAL_FLOAT: {
        /* IEEE-754 double, host byte order (memcpy). */
        return aegis_task_set_output(task, &v->as.f, sizeof(v->as.f));
    }
    case AEGIS_TOOL_VAL_STRING:
        return aegis_task_set_output(task, v->as.str.ptr, v->as.str.len);
    case AEGIS_TOOL_VAL_BYTES:
        return aegis_task_set_output(task, v->as.bytes.ptr, v->as.bytes.len);
    default:
        return AEGIS_ERR_INTERNAL;
    }
}

/* ── Executor bridge ──────────────────────────────────────────────────── */

static aegis_status_t tool_work_impl(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                     void* user)
{
    aegis_tool_job_t* job = (aegis_tool_job_t*)user;
    if (!job || !task) {
        aegis_tool_job_destroy(job);
        return AEGIS_ERR_INVALID;
    }

    aegis_tool_result_t result;
    aegis_status_t      rc = aegis_tool_execute(job->reg, job->name, job->args, token, &result);

    if (rc == AEGIS_OK) {
        rc = encode_value(&result.value, task);
        if (rc != AEGIS_OK) {
            char msg[96];
            (void)snprintf(msg, sizeof(msg), "tool '%s': output encoding failed (%d)", job->name,
                           (int)rc);
            aegis_task_set_error(task, msg);
        }
    } else {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "tool '%s': execution failed (%d)", job->name, (int)rc);
        aegis_task_set_error(task, msg);
    }

    aegis_tool_result_destroy(&result);
    aegis_tool_job_destroy(job); /* Consumed after exactly one attempt. */
    return rc;
}

aegis_work_fn aegis_tool_work_fn = tool_work_impl;

aegis_status_t aegis_tool_submit(aegis_executor_t* exec, aegis_tool_registry_t* reg,
                                 aegis_task_t* task, const char* tool_name, aegis_tool_args_t* args)
{
    aegis_tool_job_t* job = NULL;
    aegis_status_t    st  = aegis_tool_job_create(&job, reg, tool_name, args);
    if (st != AEGIS_OK) {
        aegis_tool_args_destroy(args);
        return st;
    }

    st = aegis_executor_submit(exec, task, aegis_tool_work_fn, job);
    if (st != AEGIS_OK) {
        aegis_tool_job_destroy(job); /* Also destroys the transferred args. */
    }
    return st;
}
