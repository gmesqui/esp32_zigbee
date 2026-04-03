#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------------------
// ZCL handler — processes incoming ZCL reports and command responses.
//
// All functions are called from the Zigbee task context (action handler).
// They update the device table, emit STATE log lines and drive the
// activity LED pulse.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Attribute value cache entry
// (tracks last known value to detect changes)
// ---------------------------------------------------------------------------
#define MAX_ATTR_CACHE   128

typedef struct {
    uint64_t ieee_addr;
    uint8_t  endpoint_id;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t  attr_type;
    uint8_t  value[8];     // raw bytes, enough for any scalar ZCL type
    uint32_t last_update_ms;
    bool     in_use;
} attr_cache_entry_t;

// ---------------------------------------------------------------------------
// Initialise internal cache. Call once at startup.
// ---------------------------------------------------------------------------
void zcl_handler_init(void);

// ---------------------------------------------------------------------------
// Entry points called from zigbee_core action handler
// ---------------------------------------------------------------------------

/** Process an incoming Report Attributes frame. */
esp_err_t zcl_on_report_attr(const esp_zb_zcl_report_attr_message_t *msg);

/** Process a Read Attributes response (used during interview and polling). */
esp_err_t zcl_on_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg);

/** Process an IAS Zone Enroll Request and send enrollment response. */
esp_err_t zcl_on_ias_enroll_req(const esp_zb_zcl_ias_zone_enroll_request_message_t *msg);

/** Process an IAS Zone Status Change Notification. */
esp_err_t zcl_on_ias_zone_status(const esp_zb_zcl_ias_zone_status_change_notification_message_t *msg);

/** Process a Default Response (log errors). */
esp_err_t zcl_on_default_resp(const esp_zb_zcl_cmd_default_resp_message_t *msg);

// ---------------------------------------------------------------------------
// Called when a ZCL message is received from an unknown short address
// (before IEEE resolution completes).  Saves the attr so it can be
// replayed once we know the IEEE.
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t nwk_addr;
    uint8_t  endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t  attr_type;
    uint8_t  value[8];
    uint32_t timestamp_ms;
    bool     in_use;
} pending_attr_t;

#define MAX_PENDING_ATTRS  8

/** Buffer a single attribute report from an unknown source. */
void zcl_pending_attr_save(uint16_t nwk_addr, uint8_t ep,
                            uint16_t cluster, uint16_t attr_id,
                            uint8_t attr_type, const void *value, uint8_t val_len);

/** Replay any pending attrs for a now-known IEEE. */
void zcl_pending_attr_replay(uint64_t ieee, uint16_t nwk_addr);
