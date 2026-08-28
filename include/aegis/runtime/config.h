/**
 * @file config.h
 * @brief Runtime configuration defaults.
 */
#ifndef AEGIS_CONFIG_H
#define AEGIS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Runtime configuration. */
typedef struct aegis_config {
    /** Number of worker threads (default: 4). */
    int max_workers;
    /** Event queue capacity (default: 256). */
    size_t event_queue_cap;
    /** Stop timeout in milliseconds (default: 5000). */
    long stop_timeout_ms;
    /** Optional name for diagnostics (NULL = none). */
    const char* name;
} aegis_config_t;

/**
 * Return default runtime configuration.
 *
 * @return aegis_config_t with sensible defaults.
 */
aegis_config_t aegis_config_default(void);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CONFIG_H */
