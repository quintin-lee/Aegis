/**
 * @file event.c
 * @brief Event creation and destruction.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/event/event.h"
#include "event_internal.h"
#include "lifecycle.h"
#include "aegis/common/time.h"
#include <stdlib.h>

/**
 * @brief Create an event of @p type, stamping it with the monotonic clock.
 *
 * The payload struct is copied by value (shallow); any pointed-to data
 * stays caller-owned. A NULL @p payload yields an empty payload.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param type Event type tag.
 * @param payload Payload to copy (borrowed; may be NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on failure.
 */
aegis_status_t aegis_event_create(aegis_event_t** out, aegis_event_type_t type,
                                   const aegis_event_payload_t* payload)
{
    AEGIS_CHECK_OUT(out);

    aegis_event_t* ev = (aegis_event_t*)calloc(1, sizeof(*ev));
    if (!ev) {
        return AEGIS_ERR_NOMEM;
    }

    ev->type         = type;
    ev->timestamp_ns = (uint64_t)aegis_mono_now();
    if (payload) {
        ev->payload = *payload;
    } else {
        ev->payload.data = NULL;
        ev->payload.size = 0;
    }

    *out = ev;
    return AEGIS_OK;
}

/**
 * @brief Destroy an event (payload data is borrowed, not freed).
 *
 * Safe to call with NULL (no-op).
 *
 * @param event Handle to destroy (ownership: consumed).
 */
void aegis_event_destroy(aegis_event_t* event)
{
    AEGIS_SAFE_FREE(event);
}

/**
 * @brief Read the event type tag (0 for NULL input).
 *
 * @param event Handle (borrowed).
 * @return Event type.
 */
aegis_event_type_t aegis_event_type(const aegis_event_t* event)
{
    if (!event) {
        return 0;
    }
    return event->type;
}

/**
 * @brief Read the creation timestamp in monotonic nanoseconds (0 for NULL).
 *
 * @param event Handle (borrowed).
 * @return Monotonic timestamp in ns.
 */
uint64_t aegis_event_timestamp(const aegis_event_t* event)
{
    if (!event) {
        return 0;
    }
    return event->timestamp_ns;
}

/**
 * @brief Borrow the event payload (NULL for NULL input).
 *
 * @param event Handle (borrowed).
 * @return Borrowed payload pointer, owned by the event.
 */
const aegis_event_payload_t* aegis_event_payload(const aegis_event_t* event)
{
    if (!event) {
        return NULL;
    }
    return &event->payload;
}
