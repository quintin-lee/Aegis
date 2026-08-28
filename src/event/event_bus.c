/**
 * @file event_bus.c
 * @brief Event bus — synchronous publish/subscribe dispatcher.
 *
 * Thread safety: the bus uses a recursive mutex to protect the
 * subscriber table. publish() acquires the lock, snapshots active
 * subscribers, releases the lock, then dispatches callbacks without
 * holding the lock (to avoid deadlock if a subscriber modifies the bus).
 *
 * Subscribers are invoked synchronously in subscription order.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/event/event.h"
#include "event_internal.h"
#include "lifecycle.h"
#include "aegis/common/mutex.h"
#include <stdlib.h>
#include <string.h>

aegis_status_t aegis_event_bus_create(aegis_event_bus_t** out)
{
    AEGIS_CHECK_OUT(out);

    aegis_event_bus_t* bus = (aegis_event_bus_t*)calloc(1, sizeof(*bus));
    if (!bus) {
        return AEGIS_ERR_NOMEM;
    }

    int rc = aegis_mutex_create(&bus->lock, AEGIS_MUTEX_RECURSIVE);
    if (rc != 0) {
        free(bus);
        return AEGIS_ERR_NOMEM;
    }

    *out = bus;
    return AEGIS_OK;
}

void aegis_event_bus_destroy(aegis_event_bus_t* bus)
{
    if (!bus) {
        return;
    }
    aegis_mutex_destroy(bus->lock);
    free(bus);
}

aegis_status_t aegis_event_bus_subscribe(aegis_event_bus_t* bus, aegis_event_type_t type,
                                         aegis_event_handler_fn handler, void* ctx)
{
    if (!bus || !handler) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(bus->lock);

    if (bus->n_subscribers >= AEGIS_EVENT_BUS_MAX_SUBSCRIBERS) {
        aegis_mutex_unlock(bus->lock);
        return AEGIS_ERR_BUSY;
    }

    size_t idx                    = bus->n_subscribers++;
    bus->subscribers[idx].type    = type;
    bus->subscribers[idx].handler = handler;
    bus->subscribers[idx].ctx     = ctx;
    bus->subscribers[idx].active  = 1;

    aegis_mutex_unlock(bus->lock);
    return AEGIS_OK;
}

void aegis_event_bus_unsubscribe(aegis_event_bus_t* bus, aegis_event_type_t type,
                                 aegis_event_handler_fn handler, void* ctx)
{
    if (!bus || !handler) {
        return;
    }

    aegis_mutex_lock(bus->lock);

    for (size_t i = 0; i < bus->n_subscribers; i++) {
        if (bus->subscribers[i].active && bus->subscribers[i].handler == handler &&
            bus->subscribers[i].ctx == ctx && bus->subscribers[i].type == type) {
            bus->subscribers[i].active = 0;
            break;
        }
    }

    aegis_mutex_unlock(bus->lock);
}

void aegis_event_bus_publish(aegis_event_bus_t* bus, const aegis_event_t* ev)
{
    if (!bus || !ev) {
        return;
    }

    /* Snapshot active subscribers under lock, then dispatch outside lock
     * to avoid deadlock if a subscriber calls unsubscribe() during dispatch. */
    aegis_event_subscriber_t snapshot[AEGIS_EVENT_BUS_MAX_SUBSCRIBERS];
    size_t                   n = 0;

    aegis_mutex_lock(bus->lock);
    for (size_t i = 0; i < bus->n_subscribers; i++) {
        if (bus->subscribers[i].active &&
            (bus->subscribers[i].type == 0 || bus->subscribers[i].type == ev->type)) {
            if (n < AEGIS_EVENT_BUS_MAX_SUBSCRIBERS) {
                snapshot[n++] = bus->subscribers[i];
            }
        }
    }
    aegis_mutex_unlock(bus->lock);

    for (size_t i = 0; i < n; i++) {
        snapshot[i].handler(ev, snapshot[i].ctx);
    }
}

size_t aegis_event_bus_subscriber_count(const aegis_event_bus_t* bus)
{
    if (!bus) {
        return 0;
    }
    aegis_mutex_lock(bus->lock);
    size_t count = 0;
    for (size_t i = 0; i < bus->n_subscribers; i++) {
        if (bus->subscribers[i].active) {
            count++;
        }
    }
    aegis_mutex_unlock(bus->lock);
    return count;
}
