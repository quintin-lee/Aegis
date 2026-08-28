/**
 * @file metrics.h
 * @brief Observable metrics: counters, gauges, histograms.
 *
 * Metrics provide quantitative observability into the runtime:
 *   - Counter: monotonically increasing value (e.g., tasks_completed)
 *   - Gauge: current value that can go up or down (e.g., queue_depth)
 *   - Histogram: distribution of values (e.g., task_duration_ms)
 *
 * All metrics are thread-safe via atomic operations or mutex protection.
 * Metrics do not participate in core business logic; they are purely
 * observational.
 */
#ifndef AEGIS_METRICS_H
#define AEGIS_METRICS_H

#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Metric types ──────────────────────────────────────────────────────────── */

/**
 * @brief Type of a metric.
 */
typedef enum aegis_metric_type {
    AEGIS_METRIC_COUNTER,   /**< Monotonically increasing counter.   */
    AEGIS_METRIC_GAUGE,     /**< Current value, may go up or down.    */
    AEGIS_METRIC_HISTOGRAM, /**< Distribution of observed values.    */
} aegis_metric_type_t;

/* ── Opaque handles ────────────────────────────────────────────────────────── */

/** Opaque metric handle. */
typedef struct aegis_metric aegis_metric_t;

/** Opaque metrics registry. */
typedef struct aegis_metric_registry aegis_metric_registry_t;

/* ── Registry lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Create an empty metrics registry.
 *
 * @param[out] out Receives the new registry. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_metric_registry_create(aegis_metric_registry_t** out);

/**
 * @brief Destroy a metrics registry and all registered metrics.
 *
 * Safe to call with NULL (no-op).
 *
 * @param reg Handle to destroy (ownership: consumed).
 */
void aegis_metric_registry_destroy(aegis_metric_registry_t* reg);

/* ── Counter ───────────────────────────────────────────────────────────────── */

/**
 * @brief Register a counter metric.
 *
 * Counters only increase. Use for things like "tasks completed",
 * "errors encountered", etc.
 *
 * @param reg     Registry (borrowed).
 * @param name    Unique metric name (borrowed, must remain valid).
 * @param help    Description (may be NULL).
 * @param[out] out Receives the counter handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY (duplicate name).
 */
aegis_status_t aegis_metric_registry_register_counter(aegis_metric_registry_t* reg,
                                                      const char* name, const char* help,
                                                      aegis_metric_t** out);

/**
 * @brief Increment a counter by 1.
 *
 * @param metric Counter handle (borrowed).
 */
void aegis_metric_counter_inc(aegis_metric_t* metric);

/**
 * @brief Increment a counter by delta.
 *
 * @param metric Counter handle (borrowed).
 * @param delta  Amount to increment (must be >= 0).
 */
void aegis_metric_counter_add(aegis_metric_t* metric, int64_t delta);

/**
 * @brief Get the current counter value.
 *
 * @param metric Counter handle (borrowed).
 * @return Current value.
 */
int64_t aegis_metric_counter_value(const aegis_metric_t* metric);

/* ── Gauge ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Register a gauge metric.
 *
 * Gauges represent a single numerical value that can arbitrarily go up
 * and down (e.g., queue depth, active connections, memory usage).
 *
 * @param reg     Registry (borrowed).
 * @param name    Unique metric name (borrowed).
 * @param help    Description (may be NULL).
 * @param[out] out Receives the gauge handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY.
 */
aegis_status_t aegis_metric_registry_register_gauge(aegis_metric_registry_t* reg, const char* name,
                                                    const char* help, aegis_metric_t** out);

/**
 * @brief Set a gauge to an absolute value.
 *
 * @param metric Gauge handle (borrowed).
 * @param value  New value.
 */
void aegis_metric_gauge_set(aegis_metric_t* metric, int64_t value);

/**
 * @brief Increment a gauge by delta.
 *
 * @param metric Gauge handle (borrowed).
 * @param delta  Amount to add (may be negative).
 */
void aegis_metric_gauge_add(aegis_metric_t* metric, int64_t delta);

/**
 * @brief Get the current gauge value.
 *
 * @param metric Gauge handle (borrowed).
 * @return Current value.
 */
int64_t aegis_metric_gauge_value(const aegis_metric_t* metric);

/* ── Histogram ─────────────────────────────────────────────────────────────── */

/**
 * @brief Register a histogram metric.
 *
 * Histograms track the distribution of observed values (e.g., latencies,
 * response sizes). Currently tracks sum and count; full quantile
 * computation is left to the exporter.
 *
 * @param reg     Registry (borrowed).
 * @param name    Unique metric name (borrowed).
 * @param help    Description (may be NULL).
 * @param[out] out Receives the histogram handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY.
 */
aegis_status_t aegis_metric_registry_register_histogram(aegis_metric_registry_t* reg,
                                                        const char* name, const char* help,
                                                        aegis_metric_t** out);

/**
 * @brief Record an observation in the histogram.
 *
 * @param metric Histogram handle (borrowed).
 * @param value  Observed value.
 */
void aegis_metric_histogram_observe(aegis_metric_t* metric, double value);

/**
 * @brief Get the number of observations recorded.
 *
 * @param metric Histogram handle (borrowed).
 * @return Observation count.
 */
uint64_t aegis_metric_histogram_count(const aegis_metric_t* metric);

/**
 * @brief Get the sum of all observed values.
 *
 * @param metric Histogram handle (borrowed).
 * @return Sum of observations.
 */
double aegis_metric_histogram_sum(aegis_metric_t* metric);

/* ── Introspection ─────────────────────────────────────────────────────────── */

/**
 * @brief Get the type of a metric.
 *
 * @param metric Metric handle (borrowed).
 * @return Metric type.
 */
aegis_metric_type_t aegis_metric_type(const aegis_metric_t* metric);

/**
 * @brief Get the name of a metric.
 *
 * @param metric Metric handle (borrowed).
 * @return Metric name string (borrowed).
 */
const char* aegis_metric_name(const aegis_metric_t* metric);

/**
 * @brief Number of metrics registered in the registry.
 *
 * @param reg Registry handle (borrowed).
 * @return Metric count.
 */
size_t aegis_metric_registry_count(const aegis_metric_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_METRICS_H */
