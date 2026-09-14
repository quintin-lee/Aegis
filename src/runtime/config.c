/**
 * @file config.c
 * @brief Runtime configuration implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/runtime/config.h"

#define RT_DEFAULT_WORKERS    4
#define RT_DEFAULT_QUEUE_CAP  256
#define RT_DEFAULT_TIMEOUT_MS 5000L

/**
 * @brief Return the built-in default runtime configuration.
 *
 * The default pool is sized for a small dev box (4 workers, 256-deep
 * event queue, 5 s stop timeout) and has no runtime name.
 *
 * @return Configuration with safe default values.
 */
aegis_config_t aegis_config_default(void)
{
    aegis_config_t c;
    c.max_workers     = RT_DEFAULT_WORKERS;
    c.event_queue_cap = RT_DEFAULT_QUEUE_CAP;
    c.stop_timeout_ms = RT_DEFAULT_TIMEOUT_MS;
    c.name            = NULL;
    return c;
}
