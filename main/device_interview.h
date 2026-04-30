#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "device_manager.h"
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------------------
// Device interview state machine.
//
// The interview runs entirely within the Zigbee task context using
// scheduler alarms and ZDO/ZCL callback chaining — no separate FreeRTOS
// task is needed.
//
// Interview steps (in order):
//   1. NODE_DESC   — capabilities, manufacturer_code, sleepy flag
//   2. POWER_DESC  — power mode and source level
//   3. ACTIVE_EP   — list of endpoint IDs
//   4. SIMPLE_DESC — profile, device_id, cluster lists (per endpoint)
//   5. CONFIG_REPORT — configure reporting for all supported clusters
//   6. WAIT_REPORT_CONFIG — validate Configure Reporting responses
//   7. READ_BASIC  — manufacturer, model, power_source from cluster 0x0000
//   8. READ_POWER  — battery voltage/% from cluster 0x0001 (if present)
//
// Only one interview runs at a time.  A lightweight FIFO queues device
// indices when multiple devices join simultaneously.
// ---------------------------------------------------------------------------

/** Initialise the interview subsystem. Call once at startup. */
void di_init(void);

/** Enqueue a device for interview.  Safe to call from the Zigbee task.
 *  If no interview is in progress, starts immediately. */
void di_enqueue(device_record_t *dev);

/** Enqueue a device for interview from a non-Zigbee task.
 *  Returns false if the Zigbee lock cannot be acquired in time. */
bool di_enqueue_async(device_record_t *dev);

/** Trigger async IEEE-address resolution for an unknown short address.
 *  Sends a ZDO IEEE Addr request; when the response arrives the device
 *  will be matched/created and any pending ZCL attributes replayed. */
void di_trigger_ieee_resolve(uint16_t nwk_addr);

/** Probe restored always-on devices after coordinator startup.
 *  Sends read requests to recover availability and refresh cached state. */
void di_startup_probe_known_devices(void);

/** Cancel pending interview/probe work for a device that is being removed. */
void di_forget_device(uint8_t dev_idx, uint64_t ieee);

/** Notify the interview state machine that a Configure Reporting response
 *  updated the validation session for the given device. */
void di_on_reporting_config_response(device_record_t *dev);

/** Consume a raw ZDO Active_EP_rsp seen on APS when the SDK callback times
 *  out or cannot correlate the response. Returns true when it advanced the
 *  active interview. */
bool di_on_active_ep_raw(uint16_t src_nwk, const uint8_t *asdu, uint8_t len);

/** Consume a raw ZDO Simple_Desc_rsp seen on APS when the SDK callback times
 *  out or cannot correlate the response. Returns true when it advanced the
 *  active interview. */
bool di_on_simple_desc_raw(uint16_t src_nwk, const uint8_t *asdu, uint8_t len);

// ---------------------------------------------------------------------------
// ZDO response callbacks — registered with the SDK and called from Zigbee
// task context.  Signatures match the esp_zb_zdo_*_callback_t typedefs.
// ---------------------------------------------------------------------------

void di_on_ieee_addr_resp(esp_zb_zdp_status_t zdo_status,
                           esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx);

void di_on_node_desc_resp(esp_zb_zdp_status_t zdo_status, uint16_t addr,
                           esp_zb_af_node_desc_t *node_desc, void *user_ctx);

void di_on_power_desc_resp(esp_zb_zdo_power_desc_rsp_t *power_desc,
                            void *user_ctx);

void di_on_active_ep_resp(esp_zb_zdp_status_t zdo_status, uint8_t ep_count,
                           uint8_t *ep_id_list, void *user_ctx);

void di_on_simple_desc_resp(esp_zb_zdp_status_t zdo_status,
                             esp_zb_af_simple_desc_1_1_t *simple_desc,
                             void *user_ctx);

void di_on_binding_table_resp(const esp_zb_zdo_binding_table_info_t *table_info,
                              void *user_ctx);
