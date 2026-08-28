/**
 * @file test_metrics.c
 * @brief Unit tests for the Metrics module.
 */
#include "aegis/observability/metrics.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

static void test_registry_lifecycle(void)
{
    aegis_metric_registry_t* reg = NULL;
    expect_ok(aegis_metric_registry_create(&reg), "create");
    assert(reg != NULL);
    assert(aegis_metric_registry_count(reg) == 0);
    aegis_metric_registry_destroy(reg);
    aegis_metric_registry_destroy(NULL);
}

static void test_counter(void)
{
    aegis_metric_registry_t* reg = NULL;
    aegis_metric_t* counter = NULL;
    expect_ok(aegis_metric_registry_create(&reg), "create");
    expect_ok(aegis_metric_registry_register_counter(reg, "tasks_completed", "Number of tasks completed", &counter), "register");

    assert(counter != NULL);
    assert(aegis_metric_type(counter) == AEGIS_METRIC_COUNTER);
    assert(strcmp(aegis_metric_name(counter), "tasks_completed") == 0);
    assert(aegis_metric_counter_value(counter) == 0);

    aegis_metric_counter_inc(counter);
    assert(aegis_metric_counter_value(counter) == 1);
    aegis_metric_counter_inc(counter);
    assert(aegis_metric_counter_value(counter) == 2);
    aegis_metric_counter_add(counter, 5);
    assert(aegis_metric_counter_value(counter) == 7);

    aegis_metric_registry_destroy(reg);
}

static void test_gauge(void)
{
    aegis_metric_registry_t* reg = NULL;
    aegis_metric_t* gauge = NULL;
    expect_ok(aegis_metric_registry_create(&reg), "create");
    expect_ok(aegis_metric_registry_register_gauge(reg, "queue_depth", "Current queue depth", &gauge), "register");

    assert(gauge != NULL);
    assert(aegis_metric_type(gauge) == AEGIS_METRIC_GAUGE);
    assert(aegis_metric_gauge_value(gauge) == 0);

    aegis_metric_gauge_set(gauge, 10);
    assert(aegis_metric_gauge_value(gauge) == 10);
    aegis_metric_gauge_add(gauge, 5);
    assert(aegis_metric_gauge_value(gauge) == 15);
    aegis_metric_gauge_add(gauge, -3);
    assert(aegis_metric_gauge_value(gauge) == 12);

    aegis_metric_registry_destroy(reg);
}

static void test_histogram(void)
{
    aegis_metric_registry_t* reg = NULL;
    aegis_metric_t* hist = NULL;
    expect_ok(aegis_metric_registry_create(&reg), "create");
    expect_ok(aegis_metric_registry_register_histogram(reg, "task_duration", "Task duration in ms", &hist), "register");

    assert(hist != NULL);
    assert(aegis_metric_type(hist) == AEGIS_METRIC_HISTOGRAM);
    assert(aegis_metric_histogram_count(hist) == 0);
    assert(aegis_metric_histogram_sum(hist) == 0.0);

    aegis_metric_histogram_observe(hist, 1.5);
    aegis_metric_histogram_observe(hist, 2.5);
    aegis_metric_histogram_observe(hist, 3.0);
    assert(aegis_metric_histogram_count(hist) == 3);
    assert(aegis_metric_histogram_sum(hist) == 7.0);

    aegis_metric_registry_destroy(reg);
}

static void test_duplicate_name(void)
{
    aegis_metric_registry_t* reg = NULL;
    aegis_metric_t* m1 = NULL, *m2 = NULL;
    expect_ok(aegis_metric_registry_create(&reg), "create");
    expect_ok(aegis_metric_registry_register_counter(reg, "dup", "dup metric", &m1), "register first");
    assert(aegis_metric_registry_register_counter(reg, "dup", "dup metric", &m2) == AEGIS_ERR_BUSY);

    aegis_metric_registry_destroy(reg);
}

static void test_null_safety(void)
{
    assert(aegis_metric_counter_value(NULL) == 0);
    assert(aegis_metric_gauge_value(NULL) == 0);
    assert(aegis_metric_histogram_count(NULL) == 0);
    assert(aegis_metric_histogram_sum(NULL) == 0.0);
    aegis_metric_counter_inc(NULL);
    aegis_metric_counter_add(NULL, 1);
    aegis_metric_gauge_set(NULL, 0);
    aegis_metric_gauge_add(NULL, 0);
    aegis_metric_histogram_observe(NULL, 0.0);
    aegis_metric_type(NULL);
    aegis_metric_name(NULL);
    aegis_metric_registry_count(NULL);
}

int main(void)
{
    test_null_safety();
    test_registry_lifecycle();
    test_counter();
    test_gauge();
    test_histogram();
    test_duplicate_name();

    printf("metrics: all tests passed\n");
    return 0;
}
