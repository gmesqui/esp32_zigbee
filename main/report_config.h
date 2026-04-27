#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "device_manager.h"
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------------------
// Configure reporting for a device that has completed its interview.
//
// For each endpoint × cluster combination on the device that appears in the
// "reportable clusters" table, sends a Configure Reporting command.
//
// For sleepy devices: max_interval is the critical constraint — if too long,
// we lose presence detection.  For always-on: a backup polling timer is also
// started.
//
// All calls must be made from the Zigbee task context.
// ---------------------------------------------------------------------------

/** Start configure-reporting for all relevant clusters on a device.
 *  Returns the number of Configure Reporting commands sent. */
size_t rc_configure_device(device_record_t *dev);

/** Schedule configure-reporting from a non-Zigbee task. */
void rc_configure_device_async(device_record_t *dev);

/** True when a sleepy device still has reporting work pending or in flight. */
bool rc_device_has_reporting_pending(const device_record_t *dev);

/** Configure pending sleepy reporting while the device is known to be awake.
 *  Must be called from Zigbee task context. Returns true if commands were sent. */
bool rc_configure_pending_sleepy_now(device_record_t *dev, const char *reason);

/** Clear configure-reporting sessions that did not receive all responses. */
void rc_check_reporting_timeouts(void);

/** Finish one timed-out configure-reporting session on a device. */
void rc_mark_reporting_timeout(device_record_t *dev);

/** Effective max_interval policy applied by the coordinator for reporting. */
uint16_t rc_effective_max_interval(bool is_sleepy);

/** Presence timeout derived from reporting max_interval plus a safety margin. */
uint32_t rc_presence_timeout_ms(bool is_sleepy);

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    uint16_t minimum_report_interval;
    uint16_t maximum_report_interval;
    uint32_t reportable_change;
} rc_configured_reporting_t;

/** Return configured reportings that apply to the given endpoint.
 *  If out == NULL, returns the count only. */
size_t rc_get_configured_reportings_for_endpoint(const endpoint_record_t *ep,
                                                 bool is_sleepy,
                                                 rc_configured_reporting_t *out,
                                                 size_t out_len);

/** Called by zigbee_core when a Configure Reporting Response arrives.
 *  Updates the runtime reporting-validation session for the source device. */
void rc_on_config_resp(const esp_zb_zcl_cmd_config_report_resp_message_t *msg);

/** Called by zigbee_core when a Read Reporting Configuration Response arrives. */
void rc_on_read_report_cfg_resp(const esp_zb_zcl_cmd_read_report_config_resp_message_t *msg);

/** Called by zigbee_core when a Write Attributes Response arrives. */
void rc_on_write_attr_resp(const esp_zb_zcl_cmd_write_attr_resp_message_t *msg);

/** Schedule a Read Reporting Configuration request from a non-Zigbee task.
 *  Returns false if the diagnostic queue is full or the device is invalid. */
bool rc_read_reporting_config_async(device_record_t *dev, uint8_t endpoint,
                                    uint16_t cluster_id, uint16_t attr_id);

/** Write the coordinator's own IEEE address to a device's IAS_CIE_Address
 *  attribute (0x0010 on cluster 0x0500).  Required before enrollment. */
void rc_write_ias_cie_address(uint16_t nwk_addr, uint8_t endpoint);

// ---------------------------------------------------------------------------
// Coordinator endpoint used as source in all outgoing ZCL commands.
// ---------------------------------------------------------------------------
#define COORD_ENDPOINT  1

// Presence timeout is configured at runtime as reporting max_interval + grace.
