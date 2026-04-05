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
 *  Called by device_interview when STEP_CONFIGURE_REPORTING is reached. */
void rc_configure_device(device_record_t *dev);

/** Schedule configure-reporting from a non-Zigbee task. */
void rc_configure_device_async(device_record_t *dev);

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
                                                 rc_configured_reporting_t *out,
                                                 size_t out_len);

/** Called by zigbee_core when a Configure Reporting Response arrives.
 *  Updates dev->reporting_configured if all records succeeded. */
void rc_on_config_resp(const esp_zb_zcl_cmd_config_report_resp_message_t *msg);

/** Write the coordinator's own IEEE address to a device's IAS_CIE_Address
 *  attribute (0x0010 on cluster 0x0500).  Required before enrollment. */
void rc_write_ias_cie_address(uint16_t nwk_addr, uint8_t endpoint);

// ---------------------------------------------------------------------------
// Coordinator endpoint used as source in all outgoing ZCL commands.
// ---------------------------------------------------------------------------
#define COORD_ENDPOINT  1
