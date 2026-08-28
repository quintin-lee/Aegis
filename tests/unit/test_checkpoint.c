/**
 * @file test_checkpoint.c
 * @brief Unit tests for the Checkpoint module.
 */
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/agent/agent.h"
#include "aegis/planner/plan.h"
#include "aegis/task/graph.h"
#include "aegis/task/task.h"
#include "aegis/common/cancellation/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

static aegis_plan_t* make_test_plan(const char* goal)
{
    aegis_plan_t* p = NULL;
    expect_ok(aegis_plan_create(&p, goal), "create plan");
    aegis_plan_step_spec_t spec = {0};
    spec.step_id = -1;
    spec.name = "test-step";
    spec.type = AEGIS_TASK_TYPE_COMPUTATIONAL;
    int64_t id = -1;
    expect_ok(aegis_plan_add_step(p, &spec, &id), "add step");
    return p;
}

static aegis_task_graph_t* make_test_graph(size_t count, aegis_task_state_t state)
{
    aegis_task_graph_t* g = NULL;
    expect_ok(aegis_task_graph_create(&g), "create graph");
    for (size_t i = 0; i < count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "task-%zu", i);
        aegis_task_t* t = NULL;
        expect_ok(aegis_task_create(&t, name, NULL), "create task");
        aegis_task_set_state_for_test(t, state);
        expect_ok(aegis_task_graph_add_task(g, t), "add task");
    }
    return g;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

static void test_create_destroy(void)
{
    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    assert(ckpt != NULL);
    assert(aegis_checkpoint_version(ckpt) == 0);
    aegis_checkpoint_destroy(ckpt);
    aegis_checkpoint_destroy(NULL);
}

/* ── Populate ──────────────────────────────────────────────────────────────── */

static void test_populate_empty(void)
{
    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, NULL, NULL, NULL, NULL, 0), "populate empty");
    assert(aegis_checkpoint_version(ckpt) == 1);
    assert(strcmp(aegis_checkpoint_agent_state(ckpt), "CREATED") == 0);
    assert(strcmp(aegis_checkpoint_goal(ckpt), "") == 0);
    assert(aegis_checkpoint_task_count(ckpt) == 0);
    aegis_checkpoint_destroy(ckpt);
}

static void test_populate_with_agent(void)
{
    aegis_agent_t* agent = NULL;
    expect_ok(aegis_agent_create(&agent, "test-agent"), "create agent");
    aegis_agent_set_goal(agent, "achieve world peace");

    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, "RUNNING", "achieve world peace", NULL, NULL, 5), "populate");
    assert(aegis_checkpoint_version(ckpt) == 5);
    assert(strcmp(aegis_checkpoint_goal(ckpt), "achieve world peace") == 0);

    aegis_checkpoint_destroy(ckpt);
    aegis_agent_destroy(agent);
}

static void test_populate_with_plan(void)
{
    aegis_plan_t* plan = make_test_plan("build a house");
    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, NULL, NULL, plan, NULL, 0), "populate");
    assert(aegis_checkpoint_plan_version(ckpt) > 0);
    assert(aegis_checkpoint_plan_text(ckpt) != NULL);
    assert(strlen(aegis_checkpoint_plan_text(ckpt)) > 0);

    aegis_checkpoint_destroy(ckpt);
    aegis_plan_destroy(plan);
}

static void test_populate_with_graph(void)
{
    aegis_task_graph_t* g = make_test_graph(3, AEGIS_TASK_SUCCESS);
    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, NULL, NULL, NULL, g, 0), "populate");
    assert(aegis_checkpoint_task_count(ckpt) == 3);

    const aegis_checkpoint_task_snapshot_t* snap0 = aegis_checkpoint_task_snapshot(ckpt, 0);
    assert(snap0 != NULL);
    assert(strcmp(snap0->task_name, "task-0") == 0);
    assert(snap0->task_state == (int)AEGIS_TASK_SUCCESS);

    aegis_checkpoint_destroy(ckpt);
    aegis_task_graph_destroy(g);
}

/* ── Serialization round-trip ──────────────────────────────────────────────── */

static void test_serialize_deserialize(void)
{
    aegis_agent_t* agent = NULL;
    expect_ok(aegis_agent_create(&agent, "test"), "create agent");
    aegis_agent_set_goal(agent, "solve puzzle");

    aegis_plan_t* plan = make_test_plan("solve puzzle");
    aegis_task_graph_t* g = make_test_graph(2, AEGIS_TASK_FAILED);

    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, "RUNNING", "achieve world peace", plan, g, 5), "populate");

    char* serialized = NULL;
    expect_ok(aegis_checkpoint_serialize(ckpt, &serialized), "serialize");
    assert(serialized != NULL);
    assert(strlen(serialized) > 0);

    aegis_checkpoint_t* restored = NULL;
    expect_ok(aegis_checkpoint_deserialize(serialized, &restored), "deserialize");
    assert(restored != NULL);
    assert(aegis_checkpoint_version(restored) == 5);
    assert(strcmp(aegis_checkpoint_agent_state(restored), "RUNNING") == 0);
    assert(strcmp(aegis_checkpoint_goal(restored), "achieve world peace") == 0);
    assert(aegis_checkpoint_plan_version(restored) > 0);
    assert(aegis_checkpoint_task_count(restored) == 2);

    aegis_checkpoint_destroy(restored);
    free(serialized);
    aegis_checkpoint_destroy(ckpt);
    aegis_plan_destroy(plan);
    aegis_task_graph_destroy(g);
    aegis_agent_destroy(agent);
}

/* ── Atomic write ──────────────────────────────────────────────────────────── */

static void test_write_read_roundtrip(void)
{
    aegis_agent_t* agent = NULL;
    expect_ok(aegis_agent_create(&agent, "test"), "create agent");
    aegis_agent_set_goal(agent, "test goal");

    aegis_plan_t* plan = make_test_plan("test goal");
    aegis_task_graph_t* g = make_test_graph(1, AEGIS_TASK_SUCCESS);

    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");
    expect_ok(aegis_checkpoint_populate(ckpt, "RUNNING", "test goal", NULL, NULL, 1), "populate");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/aegis_test_checkpoint_%d.chk", (int)getpid());

    expect_ok(aegis_checkpoint_write(ckpt, path, NULL), "write");

    aegis_checkpoint_t* restored = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read(path, &restored, &status);
    assert(rc == AEGIS_OK);
    assert(status == AEGIS_CHECKPOINT_OK);
    assert(restored != NULL);
    assert(aegis_checkpoint_version(restored) == 1);
    assert(strcmp(aegis_checkpoint_goal(restored), "test goal") == 0);

    aegis_checkpoint_destroy(restored);
    aegis_checkpoint_destroy(ckpt);
    aegis_plan_destroy(plan);
    aegis_task_graph_destroy(g);
    aegis_agent_destroy(agent);
    unlink(path);
}

static void test_write_cancelled(void)
{
    aegis_checkpoint_t* ckpt = NULL;
    expect_ok(aegis_checkpoint_create(&ckpt), "create");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "create token");
    aegis_cancellation_token_request_cancel(token);

    assert(aegis_checkpoint_write(ckpt, "/tmp/test.chk", token) == AEGIS_ERR_CANCELLED);

    aegis_cancellation_token_destroy(token);
    aegis_checkpoint_destroy(ckpt);
}

/* ── Crash / recovery ──────────────────────────────────────────────────────── */

static void test_read_missing(void)
{
    aegis_checkpoint_t* ckpt = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read("/tmp/nonexistent_checkpoint_12345.chk",
                                               &ckpt, &status);
    assert(rc == AEGIS_ERR_NOT_FOUND);
    assert(status == AEGIS_CHECKPOINT_MISSING);
    assert(ckpt == NULL);
}

static void test_read_corrupted(void)
{
    const char* path = "/tmp/aegis_test_corrupted.chk";
    FILE* fp = fopen(path, "w");
    assert(fp != NULL);
    fprintf(fp, "THIS IS NOT A CHECKPOINT\n");
    fclose(fp);

    aegis_checkpoint_t* ckpt = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read(path, &ckpt, &status);
    assert(rc == AEGIS_ERR_INVALID);
    assert(status == AEGIS_CHECKPOINT_CORRUPTED);
    assert(ckpt == NULL);

    unlink(path);
}

static void test_read_incomplete(void)
{
    const char* path = "/tmp/aegis_test_incomplete.chk";
    FILE* fp = fopen(path, "w");
    assert(fp != NULL);
    fprintf(fp, "AEGISCHK");
    fclose(fp);

    aegis_checkpoint_t* ckpt = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read(path, &ckpt, &status);
    assert(rc != AEGIS_OK);
    assert(ckpt == NULL);

    unlink(path);
}

static void test_read_crc_mismatch(void)
{
    /* Write a checkpoint with invalid CRC directly. */
    const char* path = "/tmp/aegis_test_crc.chk";
    FILE* fp = fopen(path, "w");
    assert(fp != NULL);
    fprintf(fp, "AEGISCHK v1\n# TS=0\n# AGENT_STATE=CREATED\n# GOAL=\n# PLAN_VERSION=0\n"
                "PLAN_START\n\nPLAN_END\n# CRC32=00000000\n");
    fclose(fp);

    aegis_checkpoint_t* restored = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read(path, &restored, &status);
    assert(rc == AEGIS_ERR_INVALID);
    assert(status == AEGIS_CHECKPOINT_CORRUPTED);
    assert(restored == NULL);

    unlink(path);
}

static void test_version_mismatch(void)
{
    const char* path = "/tmp/aegis_test_version.chk";
    FILE* fp = fopen(path, "w");
    assert(fp != NULL);
    fprintf(fp, "AEGISCHK v999\n# TS=0\n# AGENT_STATE=CREATED\n# GOAL=\n# PLAN_VERSION=0\n"
                "PLAN_START\n\nPLAN_END\n# CRC32=b373f311\n");
    fclose(fp);

    aegis_checkpoint_t* restored = NULL;
    aegis_checkpoint_status_t status = AEGIS_CHECKPOINT_OK;
    aegis_status_t rc = aegis_checkpoint_read(path, &restored, &status);
    
    assert(rc == AEGIS_ERR_INVALID);
    assert(status == AEGIS_CHECKPOINT_VERSION_MISMATCH);
    assert(restored == NULL);

    unlink(path);
}

/* ── Status string ─────────────────────────────────────────────────────────── */

static void test_status_str(void)
{
    assert(strcmp(aegis_checkpoint_status_str(AEGIS_CHECKPOINT_OK), "OK") == 0);
    assert(strcmp(aegis_checkpoint_status_str(AEGIS_CHECKPOINT_MISSING), "MISSING") == 0);
    assert(strcmp(aegis_checkpoint_status_str(AEGIS_CHECKPOINT_CORRUPTED), "CORRUPTED") == 0);
    assert(strcmp(aegis_checkpoint_status_str(AEGIS_CHECKPOINT_INCOMPLETE), "INCOMPLETE") == 0);
    assert(strcmp(aegis_checkpoint_status_str(AEGIS_CHECKPOINT_VERSION_MISMATCH),
                "VERSION_MISMATCH") == 0);
    assert(strcmp(aegis_checkpoint_status_str((aegis_checkpoint_status_t)99), "UNKNOWN") == 0);
}

/* ── Null safety ───────────────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    aegis_checkpoint_destroy(NULL);
    assert(aegis_checkpoint_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_checkpoint_serialize(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_checkpoint_deserialize(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_checkpoint_write(NULL, "/tmp/x", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_checkpoint_read(NULL, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_checkpoint_version(NULL) == 0);
    assert(aegis_checkpoint_timestamp(NULL) == 0);
    assert(strcmp(aegis_checkpoint_goal(NULL), "") == 0);
    assert(aegis_checkpoint_plan_version(NULL) == 0);
    assert(aegis_checkpoint_plan_text(NULL) == NULL);
    assert(strcmp(aegis_checkpoint_agent_state(NULL), "CREATED") == 0);
    assert(aegis_checkpoint_task_count(NULL) == 0);
    assert(aegis_checkpoint_task_snapshot(NULL, 0) == NULL);
    assert(aegis_checkpoint_status_str((aegis_checkpoint_status_t)0) != NULL);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_null_safety();
    test_create_destroy();
    test_populate_empty();
    test_populate_with_agent();
    test_populate_with_plan();
    test_populate_with_graph();
    test_serialize_deserialize();
    test_write_read_roundtrip();
    test_write_cancelled();
    test_read_missing();
    test_read_corrupted();
    test_read_incomplete();
    test_read_crc_mismatch();
    test_version_mismatch();
    test_status_str();

    printf("checkpoint: all tests passed\n");
    return 0;
}
