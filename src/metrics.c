/**
 * @file metrics.c
 * @brief Observable metrics implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/metrics.h"

#include "internal/lifecycle.h"

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

void aegis_metric_counter_inc(aegis_metric_t* metric)
{
    if (!metric) {
        return;
    }
    atomic_fetch_add(&metric->counter_value, 1);
}

void aegis_metric_counter_add(aegis_metric_t* metric, int64_t delta)
{
    if (!metric || delta <= 0) {
        return;
    }
    atomic_fetch_add(&metric->counter_value, delta);
}

int64_t aegis_metric_counter_value(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->counter_value) : 0;
}

/* ── Gauge ─────────────────────────────────────────────────────────────────── */

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

void aegis_metric_gauge_set(aegis_metric_t* metric, int64_t value)
{
    if (!metric) {
        return;
    }
    atomic_store(&metric->gauge_value, value);
}

void aegis_metric_gauge_add(aegis_metric_t* metric, int64_t delta)
{
    if (!metric) {
        return;
    }
    atomic_fetch_add(&metric->gauge_value, delta);
}

int64_t aegis_metric_gauge_value(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->gauge_value) : 0;
}

/* ── Histogram ─────────────────────────────────────────────────────────────── */

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

uint64_t aegis_metric_histogram_count(const aegis_metric_t* metric)
{
    return metric ? atomic_load(&metric->hist_count) : 0;
}

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

aegis_metric_type_t aegis_metric_type(const aegis_metric_t* metric)
{
    return metric ? metric->type : AEGIS_METRIC_COUNTER;
}

const char* aegis_metric_name(const aegis_metric_t* metric)
{
    return metric ? metric->name : "";
}

size_t aegis_metric_registry_count(const aegis_metric_registry_t* reg)
{
    return reg ? reg->count : 0;
}
