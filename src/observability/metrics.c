/**
 * @file metrics.c
 * @brief Observable metrics implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/observability/metrics.h"

#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* ── Internal metric representation ────────────────────────────────────────── */

typedef struct aegis_metric {
    aegis_metric_type_t type;
    char*               name;
    char*               help;
    /* Counter fields. */
    _Atomic(int64_t) counter_value;
    /* Gauge fields. */
    _Atomic(int64_t) gauge_value;
    /* Histogram fields (protected by mutex due to double atomic limitation). */
    _Atomic(uint64_t) hist_count;
    double            hist_sum;
    pthread_mutex_t   hist_mutex;
} aegis_metric_t;

typedef struct aegis_metric_registry {
    aegis_metric_t** metrics;
    size_t           capacity;
    size_t           count;
} aegis_metric_registry_t;

#define METRIC_REG_INITIAL_CAP 16

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_metric_t* metric_create(aegis_metric_type_t type, const char* name, const char* help)
{
    aegis_metric_t* m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->type = type;
    m->name = strdup(name);
    if (help) {
        m->help = strdup(help);
    }
    if (!m->name || (help && !m->help)) {
        free(m->help);
        free(m->name);
        free(m);
        return NULL;
    }
    atomic_store(&m->counter_value, 0);
    atomic_store(&m->gauge_value, 0);
    atomic_store(&m->hist_count, 0);
    m->hist_sum = 0.0;
    pthread_mutex_init(&m->hist_mutex, NULL);
    return m;
}

static void metric_destroy(aegis_metric_t* m)
{
    if (!m) {
        return;
    }
    pthread_mutex_destroy(&m->hist_mutex);
    free(m->name);
    free(m->help);
    free(m);
}

static aegis_metric_t* registry_find(const aegis_metric_registry_t* reg, const char* name)
{
    if (!reg || !name) {
        return NULL;
    }
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->metrics[i]->name, name) == 0) {
            return reg->metrics[i];
        }
    }
    return NULL;
}

static aegis_status_t registry_ensure_capacity(aegis_metric_registry_t* reg)
{
    if (reg->count < reg->capacity) {
        return AEGIS_OK;
    }
    size_t           new_cap     = reg->capacity * 2;
    aegis_metric_t** new_metrics = realloc(reg->metrics, sizeof(*new_metrics) * new_cap);
    if (!new_metrics) {
        return AEGIS_ERR_NOMEM;
    }
    reg->metrics  = new_metrics;
    reg->capacity = new_cap;
    return AEGIS_OK;
}

/* ── Registry lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Create an empty metric registry with an initial capacity of 16.
 *
 * The caller owns the registry and must call aegis_metric_registry_destroy.
 *
 * @param[out] out Receives the new registry; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_metric_registry_create(aegis_metric_registry_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_metric_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) {
        return AEGIS_ERR_NOMEM;
    }
    reg->capacity = METRIC_REG_INITIAL_CAP;
    reg->metrics  = calloc(reg->capacity, sizeof(*reg->metrics));
    if (!reg->metrics) {
        free(reg);
        return AEGIS_ERR_NOMEM;
    }
    *out = reg;
    return AEGIS_OK;
}

/**
 * @brief Destroy a metric registry and every metric it holds.
 *
 * NULL is a no-op. All histogram mutexes are destroyed too.
 *
 * @param[in] reg Registry to destroy, or NULL.
 */
void aegis_metric_registry_destroy(aegis_metric_registry_t* reg)
{
    if (!reg) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        metric_destroy(reg->metrics[i]);
    }
    free(reg->metrics);
    free(reg);
}

/* ── Counter ───────────────────────────────────────────────────────────────── */

/**
 * @brief Register a counter metric by name.
 *
 * Returns AEGIS_ERR_BUSY when a metric with the same name already
 * exists. The caller owns the returned handle and must not free it.
 *
 * @param[in]  reg  Registry to extend.
 * @param[in]  name Metric name (unique within the registry).
 * @param[in]  help Help text for documentation, or NULL.
 * @param[out] out  Receives the new metric handle.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation.
 */
/**
 * @brief Register a new counter metric or return an existing one by name.
 *
 * Counters only increase; calls with a negative delta are silently
 * ignored. The name must be unique within the registry — returns
 * AEGIS_ERR_BUSY when a metric of that name already exists.
 *
 * @param[in]  reg  Registry to extend (must be non-NULL).
 * @param[in]  name Unique metric identifier (must be non-NULL).
 * @param[in]  help Human-readable description, or NULL.
 * @param[out] out  Receives the new counter; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY when @p name already exists, else AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_metric_registry_register_counter(aegis_metric_registry_t* reg,
                                                      const char* name, const char* help,
                                                      aegis_metric_t** out)
{
    AEGIS_CHECK_OUT(out);
    if (!reg || !name) {
        return AEGIS_ERR_INVALID;
    }
    if (registry_find(reg, name)) {
        return AEGIS_ERR_BUSY;
    }
    if (registry_ensure_capacity(reg) != AEGIS_OK) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_metric_t* m = metric_create(AEGIS_METRIC_COUNTER, name, help);
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    reg->metrics[reg->count++] = m;
    *out                       = m;
    return AEGIS_OK;
}

/**
 * @brief Atomically increment a counter by one.
 *
 * NULL is a no-op. Safe to call from any thread.
 *
 * @param[in] metric Counter to increment.
 */
/**
 * @brief Atomically increment a counter by one.
 *
 * NULL is a no-op.
 *
 * @param[in] metric Counter to increment, or NULL.
 */
void aegis_metric_counter_inc(aegis_metric_t* metric)
{
    if (!metric) {
        return;
    }
    atomic_fetch_add(&metric->counter_value, 1);
}

/**
 * @brief Atomically add a delta to a counter (no-op when delta <= 0).
 *
 * Safe to call from any thread.
 *
 * @param[in] metric Counter to update.
 * @param[in] delta  Value to add (must be positive for effect).
 */
/**
 * @brief Atomically add a positive delta to a counter.
 *
 * Negative or zero deltas are silently dropped (counters are monotonic
 * by convention). NULL is a no-op.
 *
 * @param[in] metric Counter to update.
 * @param[in] delta  Amount to add (must be > 0 to take effect).
 */
void aegis_metric_counter_add(aegis_metric_t* metric, int64_t delta)
{
    if (!metric || delta <= 0) {
        return;
    }
    atomic_fetch_add(&metric->counter_value, delta);
}

/**
 * @brief Return the current counter value.
 *
 * @param[in] metric Counter to read, or NULL.
 * @return Current value, or 0 for NULL.
 */
/**
 * @brief Read the current counter value.
 *
 * @param[in] metric Counter, or NULL.
 * @return Current value, or 0 for NULL.
 */
int64_t aegis_metric_counter_value(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->counter_value) : 0;
}

/* ── Gauge ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Register a gauge metric by name.
 *
 * Gauge values can go up or down; they represent an instantanous state
 * (e.g. current memory usage) rather than a cumulative total.
 *
 * @param[in]  reg  Registry to extend.
 * @param[in]  name Metric name (unique within the registry).
 * @param[in]  help Help text, or NULL.
 * @param[out] out  Receives the new metric handle.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation.
 */
/**
 * @brief Register a new gauge metric or return an existing one by name.
 *
 * Gauges represent values that can go up and down (e.g. queue depth,
 * temperature). Unlike counters there is no monotonicity constraint.
 * Returns AEGIS_ERR_BUSY when a metric of that name already exists.
 *
 * @param[in]  reg  Registry to extend (must be non-NULL).
 * @param[in]  name Unique metric identifier (must be non-NULL).
 * @param[in]  help Human-readable description, or NULL.
 * @param[out] out  Receives the new gauge; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY when @p name already exists, else AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_metric_registry_register_gauge(aegis_metric_registry_t* reg, const char* name,
                                                    const char* help, aegis_metric_t** out)
{
    AEGIS_CHECK_OUT(out);
    if (!reg || !name) {
        return AEGIS_ERR_INVALID;
    }
    if (registry_find(reg, name)) {
        return AEGIS_ERR_BUSY;
    }
    if (registry_ensure_capacity(reg) != AEGIS_OK) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_metric_t* m = metric_create(AEGIS_METRIC_GAUGE, name, help);
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    reg->metrics[reg->count++] = m;
    *out                       = m;
    return AEGIS_OK;
}

/**
 * @brief Set a gauge to an absolute value.
 *
 * Safe to call from any thread. NULL is a no-op.
 *
 * @param[in] metric Gauge to update.
 * @param[in] value  New absolute value.
 */
/**
 * @brief Atomically set a gauge to an exact value.
 *
 * Unlike counters gauges can go up or down — this overwrites the
 * previous value rather than adding to it. NULL is a no-op.
 *
 * @param[in] metric Gauge to update.
 * @param[in] value  New absolute value.
 */
void aegis_metric_gauge_set(aegis_metric_t* metric, int64_t value)
{
    if (!metric) {
        return;
    }
    atomic_store(&metric->gauge_value, value);
}

/**
 * @brief Atomically increment/decrement a gauge by a delta.
 *
 * Safe to call from any thread. NULL is a no-op.
 *
 * @param[in] metric Gauge to update.
 * @param[in] delta  Value to add (may be negative).
 */
/**
 * @brief Atomically increment/decrement a gauge by a delta.
 *
 * Unlike counters, negative deltas are allowed here (gauges are not
 * monotonic). NULL is a no-op.
 *
 * @param[in] metric Gauge to update.
 * @param[in] delta  Amount to add (can be negative).
 */
void aegis_metric_gauge_add(aegis_metric_t* metric, int64_t delta)
{
    if (!metric) {
        return;
    }
    atomic_fetch_add(&metric->gauge_value, delta);
}

/**
 * @brief Return the current gauge value.
 *
 * @param[in] metric Gauge to read, or NULL.
 * @return Current value, or 0 for NULL.
 */
/**
 * @brief Read the current gauge value.
 *
 * @param[in] metric Gauge, or NULL.
 * @return Current value, or 0 for NULL.
 */
int64_t aegis_metric_gauge_value(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->gauge_value) : 0;
}

/* ── Histogram ─────────────────────────────────────────────────────────────── */

/**
 * @brief Register a histogram metric by name.
 *
 * Histograms track the distribution of observed values. Internally
 * stored as a running count+sum protected by a per-metric mutex.
 *
 * @param[in]  reg  Registry to extend.
 * @param[in]  name Metric name (unique within the registry).
 * @param[in]  help Help text, or NULL.
 * @param[out] out  Receives the new metric handle.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation.
 */
/**
 * @brief Register a new histogram metric or return an existing one by name.
 *
 * Histograms track the distribution of observed values via a count+sum
 * pair; they are not thread-safe internally (hist_mutex protects the sum
 * but the count field is atomic). Returns AEGIS_ERR_BUSY when a metric
 * of that name already exists.
 *
 * @param[in]  reg  Registry to extend (must be non-NULL).
 * @param[in]  name Unique metric identifier (must be non-NULL).
 * @param[in]  help Human-readable description, or NULL.
 * @param[out] out  Receives the new histogram; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_BUSY when @p name already exists, else AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_metric_registry_register_histogram(aegis_metric_registry_t* reg,
                                                        const char* name, const char* help,
                                                        aegis_metric_t** out)
{
    AEGIS_CHECK_OUT(out);
    if (!reg || !name) {
        return AEGIS_ERR_INVALID;
    }
    if (registry_find(reg, name)) {
        return AEGIS_ERR_BUSY;
    }
    if (registry_ensure_capacity(reg) != AEGIS_OK) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_metric_t* m = metric_create(AEGIS_METRIC_HISTOGRAM, name, help);
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    reg->metrics[reg->count++] = m;
    *out                       = m;
    return AEGIS_OK;
}

/**
 * @brief Observe one sample for a histogram.
 *
 * Increments the count atomically and adds the value to the running
 * sum under the histogram mutex. Safe to call from any thread.
 * NULL is a no-op.
 *
 * @param[in] metric Histogram to update.
 * @param[in] value  Observed value.
 */
/**
 * @brief Record one observation into a histogram.
 *
 * Thread-safe: count is atomic, sum is protected by hist_mutex. NULL is
 * a no-op.
 *
 * @param[in] metric Histogram to record into.
 * @param[in] value Observed value.
 */
void aegis_metric_histogram_observe(aegis_metric_t* metric, double value)
{
    if (!metric) {
        return;
    }
    atomic_fetch_add(&metric->hist_count, 1);
    pthread_mutex_lock(&metric->hist_mutex);
    metric->hist_sum += value;
    pthread_mutex_unlock(&metric->hist_mutex);
}

/**
 * @brief Return the number of observations recorded for a histogram.
 *
 * @param[in] metric Histogram, or NULL.
 * @return Observation count, or 0 for NULL.
 */
/**
 * @brief Read the total number of observations recorded so far.
 *
 * @param[in] metric Histogram, or NULL.
 * @return Observation count, or 0 for NULL.
 */
uint64_t aegis_metric_histogram_count(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->hist_count) : 0;
}

/**
 * @brief Return the running sum of all observed values.
 *
 * The average can be computed as sum / count. Thread-safe via the
 * per-metric mutex.
 *
 * @param[in] metric Histogram, or NULL.
 * @return Sum value, or 0.0 for NULL.
 */
/**
 * @brief Read the sum of all observed values.
 *
 * Thread-safe: the sum is protected by hist_mutex. NULL is a no-op.
 *
 * @param[in] metric Histogram, or NULL.
 * @return Sum of observations, or 0.0 for NULL.
 */
double aegis_metric_histogram_sum(aegis_metric_t* metric)
{
    if (!metric) {
        return 0.0;
    }
    pthread_mutex_lock(&metric->hist_mutex);
    double s = metric->hist_sum;
    pthread_mutex_unlock(&metric->hist_mutex);
    return s;
}

/* ── Introspection ─────────────────────────────────────────────────────────── */

/**
 * @brief Return the metric's type enum.
 *
 * @param[in] metric Metric, or NULL.
 * @return Type, or AEGIS_METRIC_COUNTER for NULL.
 */
/**
 * @brief Read the type of a metric (counter/gauge/histogram).
 *
 * Returns AEGIS_METRIC_COUNTER when @p metric is NULL (forward-compat).
 *
 * @param[in] metric Metric, or NULL.
 * @return Metric type, or AEGIS_METRIC_COUNTER for NULL.
 */
aegis_metric_type_t aegis_metric_type(const aegis_metric_t* metric)
{
    return metric ? metric->type : AEGIS_METRIC_COUNTER;
}

/**
 * @brief Borrow the metric name string.
 *
 * @param[in] metric Metric, or NULL.
 * @return Name text, or "" for NULL.
 */
/**
 * @brief Read the name of a metric.
 *
 * Returns an empty string when @p metric is NULL (rather than NULL).
 *
 * @param[in] metric Metric, or NULL.
 * @return Name string, or "" for NULL.
 */
const char* aegis_metric_name(const aegis_metric_t* metric)
{
    return metric ? metric->name : "";
}

/**
 * @brief Return the number of metrics registered in the registry.
 *
 * @param[in] reg Registry, or NULL.
 * @return Metric count, or 0 for NULL.
 */
/**
 * @brief Read the number of metrics currently registered.
 *
 * @param[in] reg Registry, or NULL.
 * @return Metric count, or 0 for NULL.
 */
size_t aegis_metric_registry_count(const aegis_metric_registry_t* reg)
{
    return reg ? reg->count : 0;
}
