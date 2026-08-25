/**
 * @file event.c
 * @brief Event creation and destruction.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/event.h"
#include "internal/event_internal.h"
#include "internal/lifecycle.h"
#include "aegis/common/time.h"
#include <stdlib.h>

aegis_status_t aegis_event_create(aegis_event_t** out,
                                   aegis_event_type_t type,
                                   const aegis_event_payload_t* payload) {
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
        ev->payload.data   = NULL;
        ev->payload.size   = 0;
    }

    *out = ev;
    return AEGIS_OK;
}

void aegis_event_destroy(aegis_event_t* event) {
    AEGIS_SAFE_FREE(event);
}

aegis_event_type_t aegis_event_type(const aegis_event_t* event) {
    if (!event) {
        return 0;
    }
    return event->type;
}

uint64_t aegis_event_timestamp(const aegis_event_t* event) {
    if (!event) {
        return 0;
    }
    return event->timestamp_ns;
}

const aegis_event_payload_t* aegis_event_payload(const aegis_event_t* event) {
    if (!event) {
        return NULL;
    }
    return &event->payload;
}
