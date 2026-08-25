/**
 * @file test_agent.c
 * @brief Tests for agent state machine and lifecycle.
 */
#include "aegis/agent.h"
#include "aegis/event.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Helper: count state transitions received ─────────────────────────────── */

static int g_state_events_received = 0;
static aegis_agent_state_t g_last_state = AEGIS_AGENT_CREATED;

static void state_event_handler(const aegis_event_t* ev, void* ctx) {
    (void)ctx;
    if (aegis_event_type(ev) == 0x1000) { /* STATE_CHANGE */
        const aegis_agent_state_t* s = (const aegis_agent_state_t*)
            aegis_event_payload(ev)->data;
        if (s) {
            g_last_state = *s;
            g_state_events_received++;
        }
    }
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_create_destroy(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "test") == AEGIS_OK);
    assert(agent != NULL);
    assert(aegis_agent_name(agent) != NULL);
    assert(strcmp(aegis_agent_name(agent), "test") == 0);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_CREATED);
    assert(aegis_agent_get_goal(agent) == NULL);
    assert(aegis_agent_event_bus(agent) != NULL);
    aegis_agent_destroy(agent);
}

static void test_null_create(void) {
    aegis_agent_t* a = NULL;
    assert(aegis_agent_create(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_agent_create(&a, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_create(&a, "") == AEGIS_ERR_INVALID);
}

static void test_null_operations(void) {
    assert(aegis_agent_init(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_start(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_pause(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_resume(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_cancel(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_agent_join(NULL, 0) == AEGIS_ERR_INVALID);
    assert(aegis_agent_name(NULL) == NULL);
    assert(aegis_agent_get_goal(NULL) == NULL);
    assert(aegis_agent_event_bus(NULL) == NULL);
    aegis_agent_destroy(NULL); /* safe */
}

static void test_full_lifecycle(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "lifecycle") == AEGIS_OK);

    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_READY);

    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_RUNNING);

    assert(aegis_agent_pause(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_PAUSED);

    assert(aegis_agent_resume(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_RUNNING);

    assert(aegis_agent_cancel(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_CANCELLED);

    aegis_agent_destroy(agent);
}

static void test_idempotent_init(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "idemp") == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_OK); /* still OK */
    aegis_agent_destroy(agent);
}

static void test_idempotent_start(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "idemp") == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_start(agent) == AEGIS_OK); /* still OK */
    aegis_agent_destroy(agent);
}

static void test_invalid_transitions(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "invalid") == AEGIS_OK);

    /* Cannot start from CREATED */
    assert(aegis_agent_start(agent) == AEGIS_ERR_INVALID);
    /* Cannot pause from CREATED */
    assert(aegis_agent_pause(agent) == AEGIS_ERR_INVALID);
    /* Cannot resume from CREATED */
    assert(aegis_agent_resume(agent) == AEGIS_ERR_INVALID);
    /* Cannot cancel from CREATED */
    assert(aegis_agent_cancel(agent) == AEGIS_ERR_INVALID);

    /* Init must happen first */
    assert(aegis_agent_init(agent) == AEGIS_OK);
    /* Cannot init again */
    assert(aegis_agent_init(agent) == AEGIS_OK); /* idempotent */

    /* Cannot init from RUNNING */
    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_ERR_INVALID);

    aegis_agent_destroy(agent);
}

static void test_goal_set_get(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "goal_test") == AEGIS_OK);

    aegis_agent_set_goal(agent, "solve world hunger");
    assert(strcmp(aegis_agent_get_goal(agent), "solve world hunger") == 0);

    aegis_agent_set_goal(agent, NULL);
    assert(aegis_agent_get_goal(agent) == NULL);

    aegis_agent_destroy(agent);
}

static void test_event_bus_subscription(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "event_test") == AEGIS_OK);

    g_state_events_received = 0;
    aegis_event_bus_subscribe(aegis_agent_event_bus(agent),
                               0x1000, state_event_handler, NULL);

    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(g_state_events_received >= 2); /* INITIALIZING + READY */

    aegis_event_bus_unsubscribe(aegis_agent_event_bus(agent),
                                 0x1000, state_event_handler, NULL);
    assert(aegis_event_bus_subscriber_count(aegis_agent_event_bus(agent)) == 0);

    aegis_agent_destroy(agent);
}

static void test_state_transitions_complete_coverage(void) {
    /* Verify every state can be reached and tested */
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "coverage") == AEGIS_OK);

    assert(aegis_agent_state(agent) == AEGIS_AGENT_CREATED);
    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_READY);
    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_RUNNING);
    assert(aegis_agent_pause(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_PAUSED);
    assert(aegis_agent_resume(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_RUNNING);
    assert(aegis_agent_cancel(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_CANCELLED);

    /* Cannot transition from terminal state */
    assert(aegis_agent_start(agent) == AEGIS_ERR_INVALID);
    assert(aegis_agent_pause(agent) == AEGIS_ERR_INVALID);
    assert(aegis_agent_resume(agent) == AEGIS_ERR_INVALID);
    assert(aegis_agent_cancel(agent) == AEGIS_ERR_INVALID);

    aegis_agent_destroy(agent);
}

static void test_cancel_from_paused(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "cancel_paused") == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_pause(agent) == AEGIS_OK);
    assert(aegis_agent_cancel(agent) == AEGIS_OK);
    assert(aegis_agent_state(agent) == AEGIS_AGENT_CANCELLED);
    aegis_agent_destroy(agent);
}

static void test_join_after_cancel(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "join_test") == AEGIS_OK);
    assert(aegis_agent_init(agent) == AEGIS_OK);
    assert(aegis_agent_start(agent) == AEGIS_OK);
    assert(aegis_agent_cancel(agent) == AEGIS_OK);
    assert(aegis_agent_join(agent, 1000) == AEGIS_OK);
    aegis_agent_destroy(agent);
}

static void test_long_goal_string(void) {
    aegis_agent_t* agent = NULL;
    assert(aegis_agent_create(&agent, "long_goal") == AEGIS_OK);

    /* Goal exactly at buffer limit */
    char long_goal[4096];
    memset(long_goal, 'A', sizeof(long_goal) - 1);
    long_goal[sizeof(long_goal) - 1] = '\0';
    aegis_agent_set_goal(agent, long_goal);
    assert(aegis_agent_get_goal(agent) != NULL);
    assert(strlen(aegis_agent_get_goal(agent)) == sizeof(long_goal) - 1);

    aegis_agent_destroy(agent);
}

int main(void) {
    test_create_destroy();
    test_null_create();
    test_null_operations();
    test_full_lifecycle();
    test_idempotent_init();
    test_idempotent_start();
    test_invalid_transitions();
    test_goal_set_get();
    test_event_bus_subscription();
    test_state_transitions_complete_coverage();
    test_cancel_from_paused();
    test_join_after_cancel();
    test_long_goal_string();

    printf("agent state machine test passed\n");
    return 0;
}
