/**
 * @file event_internal.h
 * @brief Internal event and event bus struct layouts.
 *
 * NOT part of the public API. Included by event.c and event_bus.c only.
 */
#ifndef AEGIS_EVENT_INTERNAL_H
#define AEGIS_EVENT_INTERNAL_H

#include "aegis/event.h"
#include <stdint.h>
#include <stddef.h>

/** Maximum length of a subscriber entry in the bus subscriber table. */
#define AEGIS_EVENT_BUS_MAX_SUBSCRIBERS 256

/* ── Event ─────────────────────────────────────────────────────────────────── */

struct aegis_event {
    aegis_event_type_t      type;
    uint64_t                timestamp_ns;
    aegis_event_payload_t   payload;
};

/* ── Event Bus ─────────────────────────────────────────────────────────────── */

/** Single subscriber entry. */
typedef struct aegis_event_subscriber {
    aegis_event_type_t         type;          /**< 0 = match all */
    aegis_event_handler_fn     handler;       /**< callback */
    void*                      ctx;           /**< user context */
    int                        active;        /**< 1 = subscribed, 0 = unsubscribed */
} aegis_event_subscriber_t;

/** Event bus — thread-safe pub/sub dispatcher. */
struct aegis_event_bus {
    aegis_event_subscriber_t  subscribers[AEGIS_EVENT_BUS_MAX_SUBSCRIBERS];
    size_t                    n_subscribers;
};

#endif /* AEGIS_EVENT_INTERNAL_H */
