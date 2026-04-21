#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Zigbee event bus — neutral layer between Zigbee stack code and consumers
//
// Zigbee task code calls zb_events_emit() to notify all registered handlers.
// Handlers are called synchronously in the same task context, so they MUST
// be fast and non-blocking (queue work to other tasks if needed).
// ---------------------------------------------------------------------------

typedef enum {
    ZB_EVT_DEVICE_JOINED,     // device announced / rejoined
    ZB_EVT_DEVICE_LEAVE,      // device left the network
    ZB_EVT_INTERVIEW,         // interview lifecycle (started/successful/failed)
    ZB_EVT_ATTR_CHANGED,      // ZCL attribute value changed
    ZB_EVT_AVAILABILITY,      // device online/offline state changed
    ZB_EVT_PERMIT_JOIN,       // permit-join window opened or closed
} zb_evt_type_t;

// Friendly name length matches device_manager.h FRIENDLY_NAME_LEN
#define ZB_EVT_NAME_LEN  33

typedef struct {
    zb_evt_type_t type;

    // Device identity (set for most events)
    uint64_t ieee;
    char     friendly_name[ZB_EVT_NAME_LEN];

    // ZB_EVT_AVAILABILITY, ZB_EVT_DEVICE_JOINED
    bool     online;
    uint8_t  lqi;
    bool     has_lqi;

    // ZB_EVT_ATTR_CHANGED
    uint8_t  endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t  attr_type;
    uint8_t  value[8];

    // ZB_EVT_INTERVIEW: points to a string literal ("started"/"successful"/"failed")
    const char *interview_status;

    // ZB_EVT_PERMIT_JOIN
    uint8_t  permit_join_duration;

} zb_event_t;

typedef void (*zb_event_handler_t)(const zb_event_t *evt);

#define ZB_EVENTS_MAX_HANDLERS  4

/** Call once at startup before any emit. */
void zb_events_init(void);

/** Register a handler (up to ZB_EVENTS_MAX_HANDLERS). */
void zb_events_register(zb_event_handler_t handler);

/**
 * Emit an event to all registered handlers.
 * Non-blocking — handlers must not block the caller.
 * Safe to call from Zigbee task context.
 */
void zb_events_emit(const zb_event_t *evt);
