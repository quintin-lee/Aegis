/**
 * @file test_event_bus.c
 * @brief Tests for event bus publish/subscribe/unsubscribe.
 */
#include "aegis/event/event.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Test helpers ──────────────────────────────────────────────────────────── */

static int                g_event_count     = 0;
static aegis_event_type_t g_last_event_type = 0;
static int                g_ctx_value       = -1;

static void counting_handler(const aegis_event_t* ev, void* ctx)
{
    g_event_count++;
    g_last_event_type = aegis_event_type(ev);
    if (ctx) {
        g_ctx_value = *(int*)ctx;
    }
}

static void reset_counters(void)
{
    g_event_count     = 0;
    g_last_event_type = 0;
    g_ctx_value       = -1;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_create_destroy(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);
    assert(bus != NULL);
    assert(aegis_event_bus_subscriber_count(bus) == 0);
    aegis_event_bus_destroy(bus);
}

static void test_null_operations(void)
{
    assert(aegis_event_bus_subscribe(NULL, 0, counting_handler, NULL) == AEGIS_ERR_INVALID);
    aegis_event_bus_unsubscribe(NULL, 0, counting_handler, NULL);
    aegis_event_bus_publish(NULL, NULL);
    assert(aegis_event_bus_subscriber_count(NULL) == 0);
    aegis_event_bus_destroy(NULL);
}

static void test_publish_no_subscribers(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    aegis_event_payload_t payload = {.data = "hello", .size = 6};
    aegis_event_t*        ev      = NULL;
    assert(aegis_event_create(&ev, 100, &payload) == AEGIS_OK);

    aegis_event_bus_publish(bus, ev); /* no crash */
    assert(g_event_count == 0);

    aegis_event_destroy(ev);
    aegis_event_bus_destroy(bus);
}

static void test_subscribe_and_publish(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    int ctx_val = 42;
    assert(aegis_event_bus_subscribe(bus, 100, counting_handler, &ctx_val) == AEGIS_OK);
    assert(aegis_event_bus_subscriber_count(bus) == 1);

    aegis_event_payload_t payload = {.data = "msg", .size = 4};
    aegis_event_t*        ev      = NULL;
    assert(aegis_event_create(&ev, 100, &payload) == AEGIS_OK);

    reset_counters();
    aegis_event_bus_publish(bus, ev);
    assert(g_event_count == 1);
    assert(g_last_event_type == 100);
    assert(g_ctx_value == 42);

    aegis_event_destroy(ev);
    aegis_event_bus_destroy(bus);
}

static void test_wildcard_subscription(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    /* Subscribe to type 0 (all events) */
    assert(aegis_event_bus_subscribe(bus, 0, counting_handler, NULL) == AEGIS_OK);

    aegis_event_payload_t p1  = {.data = "a", .size = 2};
    aegis_event_t*        ev1 = NULL;
    assert(aegis_event_create(&ev1, 1, &p1) == AEGIS_OK);

    aegis_event_payload_t p2  = {.data = "b", .size = 2};
    aegis_event_t*        ev2 = NULL;
    assert(aegis_event_create(&ev2, 2, &p2) == AEGIS_OK);

    reset_counters();
    aegis_event_bus_publish(bus, ev1);
    assert(g_event_count == 1);

    aegis_event_bus_publish(bus, ev2);
    assert(g_event_count == 2);

    aegis_event_destroy(ev1);
    aegis_event_destroy(ev2);
    aegis_event_bus_destroy(bus);
}

static void test_type_filtering(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    /* Subscribe only to type 100 */
    assert(aegis_event_bus_subscribe(bus, 100, counting_handler, NULL) == AEGIS_OK);

    aegis_event_payload_t p100  = {.data = "x", .size = 2};
    aegis_event_t*        ev100 = NULL;
    assert(aegis_event_create(&ev100, 100, &p100) == AEGIS_OK);

    aegis_event_payload_t p200  = {.data = "y", .size = 2};
    aegis_event_t*        ev200 = NULL;
    assert(aegis_event_create(&ev200, 200, &p200) == AEGIS_OK);

    reset_counters();
    aegis_event_bus_publish(bus, ev100);
    assert(g_event_count == 1);

    aegis_event_bus_publish(bus, ev200);
    assert(g_event_count == 1); /* not incremented — wrong type */

    aegis_event_destroy(ev100);
    aegis_event_destroy(ev200);
    aegis_event_bus_destroy(bus);
}

static void test_unsubscribe(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    int ctx = 1;
    assert(aegis_event_bus_subscribe(bus, 50, counting_handler, &ctx) == AEGIS_OK);
    assert(aegis_event_bus_subscriber_count(bus) == 1);

    aegis_event_bus_unsubscribe(bus, 50, counting_handler, &ctx);
    assert(aegis_event_bus_subscriber_count(bus) == 0);

    aegis_event_payload_t p  = {.data = "z", .size = 2};
    aegis_event_t*        ev = NULL;
    assert(aegis_event_create(&ev, 50, &p) == AEGIS_OK);

    reset_counters();
    aegis_event_bus_publish(bus, ev);
    assert(g_event_count == 0); /* no subscribers */

    aegis_event_destroy(ev);
    aegis_event_bus_destroy(bus);
}

static void test_unsubscribe_nonexistent(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    /* Unsubscribing when nothing subscribed is a no-op */
    aegis_event_bus_unsubscribe(bus, 999, counting_handler, NULL);
    assert(aegis_event_bus_subscriber_count(bus) == 0);

    aegis_event_bus_destroy(bus);
}

static void test_multiple_subscribers(void)
{
    aegis_event_bus_t* bus = NULL;
    assert(aegis_event_bus_create(&bus) == AEGIS_OK);

    int ctx1 = 1, ctx2 = 2;
    assert(aegis_event_bus_subscribe(bus, 1, counting_handler, &ctx1) == AEGIS_OK);
    assert(aegis_event_bus_subscribe(bus, 1, counting_handler, &ctx2) == AEGIS_OK);
    assert(aegis_event_bus_subscriber_count(bus) == 2);

    aegis_event_payload_t p  = {.data = "m", .size = 2};
    aegis_event_t*        ev = NULL;
    assert(aegis_event_create(&ev, 1, &p) == AEGIS_OK);

    reset_counters();
    aegis_event_bus_publish(bus, ev);
    assert(g_event_count == 2); /* both subscribers called */

    aegis_event_destroy(ev);
    aegis_event_bus_destroy(bus);
}

static void test_event_creation(void)
{
    aegis_event_payload_t payload = {.data = "test", .size = 5};
    aegis_event_t*        ev      = NULL;
    assert(aegis_event_create(&ev, 42, &payload) == AEGIS_OK);
    assert(ev != NULL);
    assert(aegis_event_type(ev) == 42);
    assert(aegis_event_timestamp(ev) > 0);
    assert(aegis_event_payload(ev) != NULL);
    assert(aegis_event_payload(ev)->size == 5);
    aegis_event_destroy(ev);
}

static void test_event_null_payload(void)
{
    aegis_event_t* ev = NULL;
    assert(aegis_event_create(&ev, 7, NULL) == AEGIS_OK);
    assert(aegis_event_type(ev) == 7);
    assert(aegis_event_payload(ev) != NULL);
    assert(aegis_event_payload(ev)->data == NULL);
    assert(aegis_event_payload(ev)->size == 0);
    aegis_event_destroy(ev);
}

static void test_event_null_create(void)
{
    assert(aegis_event_create(NULL, 1, NULL) == AEGIS_ERR_INVALID);
}

static void test_null_event_accessors(void)
{
    assert(aegis_event_type(NULL) == 0);
    assert(aegis_event_timestamp(NULL) == 0);
    assert(aegis_event_payload(NULL) == NULL);
    aegis_event_destroy(NULL);
}

int main(void)
{
    test_create_destroy();
    test_null_operations();
    test_publish_no_subscribers();
    test_subscribe_and_publish();
    test_wildcard_subscription();
    test_type_filtering();
    test_unsubscribe();
    test_unsubscribe_nonexistent();
    test_multiple_subscribers();
    test_event_creation();
    test_event_null_payload();
    test_event_null_create();
    test_null_event_accessors();

    printf("event bus test passed\n");
    return 0;
}
