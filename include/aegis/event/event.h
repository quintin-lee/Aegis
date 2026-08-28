/**
 * @file event.h
 * @brief Event types and Event Bus API.
 *
 * Events are immutable timestamped records emitted by agents or
 * subsystems. The event bus provides synchronous publish/subscribe
 * with mutex-protected access for concurrent safety.
 *
 * Event Bus semantics:
 *   - publish() is synchronous: it calls all subscribers before returning
 *   - subscribers must not block indefinitely
 *   - unsubscribe() removes the subscriber from the bus atomically
 *   - The bus does not retain events after dispatch; subscribers
 *     receive a borrowed pointer to the event
 *
 * No circular dependency: agent.h includes event.h, but event.h
 * does NOT include agent.h. The event bus is unaware of agents.
 */
#ifndef AEGIS_EVENT_H
#define AEGIS_EVENT_H

#include "aegis/types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unique event type identifier.
 *
 * Each event type represents a category of occurrence (e.g., state
 * transition, goal progress, error). Subscribers filter by type.
 */
typedef uint32_t aegis_event_type_t;

/**
 * @brief Event payload — opaque data associated with an event.
 *
 * The payload is a borrowed pointer; the event does NOT own it.
 * The caller must ensure the payload remains valid for the
 * duration of subscriber processing.
 */
typedef struct aegis_event_payload {
    const void* data;
    size_t      size;
} aegis_event_payload_t;

/**
 * @brief An event — an immutable timestamped occurrence.
 */
typedef struct aegis_event aegis_event_t;

/**
 * @brief Subscriber callback signature.
 *
 * @param event  The event being dispatched (borrowed, must not be modified).
 * @param ctx    Opaque context provided at subscription time.
 */
typedef void (*aegis_event_handler_fn)(const aegis_event_t* event, void* ctx);

/**
 * @brief Event bus — pub/sub dispatcher.
 */
typedef struct aegis_event_bus aegis_event_bus_t;

/* ── Event ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Create a new event.
 *
 * @param[out]  out        Receives the event handle. Ownership: transferred.
 * @param[in]   type       Event type identifier.
 * @param[in]   payload    Payload data (may be NULL if size is 0).
 * @param[in]   payload_size  Size of payload in bytes.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_event_create(aegis_event_t** out, aegis_event_type_t type,
                                  const aegis_event_payload_t* payload);

/**
 * @brief Destroy an event and release its storage.
 *
 * Safe to call with NULL (no-op).
 *
 * @param event Handle to destroy (ownership: consumed).
 */
void aegis_event_destroy(aegis_event_t* event);

/**
 * @brief Get the event type.
 *
 * @param event Event handle (borrowed).
 * @return Event type identifier.
 */
aegis_event_type_t aegis_event_type(const aegis_event_t* event);

/**
 * @brief Get the event timestamp (nanoseconds since epoch).
 *
 * @param event Event handle (borrowed).
 * @return Timestamp in nanoseconds.
 */
uint64_t aegis_event_timestamp(const aegis_event_t* event);

/**
 * @brief Get the event payload.
 *
 * @param event Event handle (borrowed).
 * @return Payload pointer (may be NULL).
 */
const aegis_event_payload_t* aegis_event_payload(const aegis_event_t* event);

/* ── Event Bus ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create an event bus.
 *
 * @param[out] out  Receives the bus handle. Ownership: transferred.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_event_bus_create(aegis_event_bus_t** out);

/**
 * @brief Destroy an event bus.
 *
 * Detaches all subscribers before freeing. Safe to call with NULL.
 *
 * @param bus Handle to destroy (ownership: consumed).
 */
void aegis_event_bus_destroy(aegis_event_bus_t* bus);

/**
 * @brief Subscribe to events of a given type.
 *
 * The subscriber is invoked synchronously during publish().
 * Multiple subscriptions with the same handler and context are allowed.
 *
 * @param bus     Event bus (borrowed).
 * @param type    Event type to subscribe to (0 = all types).
 * @param handler Callback invoked for matching events (must not be NULL).
 * @param ctx     Opaque context passed to handler (may be NULL).
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_event_bus_subscribe(aegis_event_bus_t* bus, aegis_event_type_t type,
                                         aegis_event_handler_fn handler, void* ctx);

/**
 * @brief Unsubscribe a handler from the bus.
 *
 * Removes the FIRST subscription matching both handler and ctx.
 * If no match is found, this is a no-op (no error returned).
 *
 * @param bus     Event bus (borrowed).
 * @param type    Event type that was subscribed (0 = all).
 * @param handler Callback to remove (must not be NULL).
 * @param ctx     Context that was passed at subscription.
 */
void aegis_event_bus_unsubscribe(aegis_event_bus_t* bus, aegis_event_type_t type,
                                 aegis_event_handler_fn handler, void* ctx);

/**
 * @brief Publish an event to all matching subscribers.
 *
 * Synchronous: all subscribers are invoked before this function returns.
 * Subscribers are invoked in subscription order.
 *
 * @param bus  Event bus (borrowed).
 * @param ev   Event to publish (borrowed; must remain valid during dispatch).
 */
void aegis_event_bus_publish(aegis_event_bus_t* bus, const aegis_event_t* ev);

/**
 * @brief Return the number of active subscribers.
 *
 * @param bus Event bus (borrowed; may be NULL → returns 0).
 * @return Subscriber count.
 */
size_t aegis_event_bus_subscriber_count(const aegis_event_bus_t* bus);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_EVENT_H */
