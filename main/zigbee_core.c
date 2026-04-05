#include "zigbee_core.h"
#include "zb_events.h"
#include "device_manager.h"
#include "device_interview.h"
#include "zcl_handler.h"
#include "report_config.h"
#include "nvs_cache.h"
#include "led_driver.h"
#include "button_handler.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"
#include "esp_check.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// Coordinator configuration
// ---------------------------------------------------------------------------

#define COORD_ENDPOINT          1
#define HA_PROFILE_ID           0x0104
#define HOME_GATEWAY_DEVICE_ID  0x0050

// Maximum children (routers + end devices) the coordinator manages
#define MAX_CHILDREN            20
#define OVERALL_NETWORK_SIZE    (MAX_DEVICES + 1)

// Maintenance period: NVS flush + presence check (ms)
#define MAINTENANCE_PERIOD_MS   10000u

static volatile bool s_network_ready = false;

// ---------------------------------------------------------------------------
// Maintenance alarm — fires every MAINTENANCE_PERIOD_MS
// ---------------------------------------------------------------------------

static void maintenance_alarm(uint8_t param)
{
    (void)param;
    nvs_cache_save_dirty();
    dm_check_presence();
    // Reschedule
    esp_zb_scheduler_alarm(maintenance_alarm, 0, MAINTENANCE_PERIOD_MS);
}

// ---------------------------------------------------------------------------
// Signal handler — called from Zigbee task context
// ---------------------------------------------------------------------------

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    esp_zb_app_signal_type_t sig_type = *signal_struct->p_app_signal;
    esp_err_t err = signal_struct->esp_err_status;

    switch (sig_type) {
        // ----- Stack / BDB startup -----

        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ZB_LOG("SIGNAL SKIP_STARTUP — initialising BDB");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            if (err == ESP_OK) {
                ZB_LOG("SIGNAL FIRST_START — forming new network");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ZB_LOG("SIGNAL FIRST_START err=0x%X — retrying", err);
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err == ESP_OK) {
                uint8_t  ch    = esp_zb_get_current_channel();
                uint16_t pan   = esp_zb_get_pan_id();
                ZB_LOG("SIGNAL FORMATION OK channel=%u pan_id=0x%04X", ch, pan);
                s_network_ready = true;
                button_handler_set_stack_ready(true);
                esp_zb_scheduler_alarm(maintenance_alarm, 0, MAINTENANCE_PERIOD_MS);
                di_startup_probe_known_devices();
            } else {
                ZB_LOG("SIGNAL FORMATION FAILED err=0x%X — retrying", err);
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err == ESP_OK) {
                uint8_t  ch  = esp_zb_get_current_channel();
                uint16_t pan = esp_zb_get_pan_id();
                ZB_LOG("SIGNAL DEVICE_REBOOT — network restored channel=%u pan_id=0x%04X",
                       ch, pan);
                s_network_ready = true;
                button_handler_set_stack_ready(true);
                esp_zb_scheduler_alarm(maintenance_alarm, 0, MAINTENANCE_PERIOD_MS);
                di_startup_probe_known_devices();
            } else {
                ZB_LOG("SIGNAL DEVICE_REBOOT err=0x%X — forming new network", err);
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            ZB_LOG("SIGNAL STEERING %s", err == ESP_OK ? "OK" : "failed");
            break;

        // ----- Join / leave events -----

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        {
            led_trigger_activity_pulse();
            esp_zb_zdo_signal_device_annce_params_t *p =
                (esp_zb_zdo_signal_device_annce_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);

            uint64_t ieee = 0;
            memcpy(&ieee, p->ieee_addr, 8);
            uint16_t nwk = p->device_short_addr;

            char ibuf[20];
            utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
            ZB_LOG("SIGNAL DEVICE_ANNCE ieee=%s nwk=0x%04X cap=0x%02X",
                   ibuf, nwk, p->capability);

            dm_lock();
            device_record_t *dev = dm_get_or_create(ieee, nwk);
            if (dev) {
                dm_update_nwk(dev, nwk);
                dm_set_online(dev, true);

                bool needs_interview =
                    (dev->state == DEV_STATE_NEW ||
                     dev->state == DEV_STATE_FAILED);

                dm_unlock();

                if (needs_interview) {
                    di_enqueue(dev);
                } else {
                    ZB_LOG("DEVICE %s rejoined (already known, state=%s)",
                           dm_display_name(dev),
                           utils_device_state_name((int)dev->state));
                    // Re-configure reporting if it was lost
                    if (!dev->reporting_configured) {
                        di_enqueue(dev);
                    }
                }

                // Notify event consumers (e.g. MQTT bridge)
                zb_event_t evt = {
                    .type   = ZB_EVT_DEVICE_JOINED,
                    .ieee   = ieee,
                    .online = true,
                };
                strncpy(evt.friendly_name, dev->friendly_name,
                        ZB_EVT_NAME_LEN - 1);
                zb_events_emit(&evt);
            } else {
                dm_unlock();
                ZB_LOG("ERROR: device table full — cannot add %s", ibuf);
            }
            break;
        }

        case ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED:
        {
            led_trigger_activity_pulse();
            esp_zb_nwk_signal_device_associated_params_t *p =
                (esp_zb_nwk_signal_device_associated_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            char iassoc[20];
            uint64_t iassoc_ieee = 0;
            memcpy(&iassoc_ieee, p->device_addr, 8);
            utils_ieee_to_str(iassoc_ieee, iassoc, sizeof(iassoc));
            ZB_LOG("SIGNAL DEVICE_ASSOCIATED ieee=%s", iassoc);
            break;
        }

        case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED:
        {
            led_trigger_activity_pulse();
            esp_zb_zdo_signal_device_authorized_params_t *p =
                (esp_zb_zdo_signal_device_authorized_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            char ibuf[20];
            uint64_t ieee = 0;
            memcpy(&ieee, p->long_addr, 8);
            utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
            ZB_LOG("SIGNAL DEVICE_AUTHORIZED ieee=%s nwk=0x%04X type=%u status=%u",
                   ibuf, p->short_addr, p->authorization_type,
                   p->authorization_status);
            break;
        }

        case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE:
        {
            led_trigger_activity_pulse();
            esp_zb_zdo_signal_device_update_params_t *p =
                (esp_zb_zdo_signal_device_update_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            char ibuf[20];
            uint64_t ieee = 0;
            memcpy(&ieee, p->long_addr, 8);
            utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
            ZB_LOG("SIGNAL DEVICE_UPDATE ieee=%s nwk=0x%04X status=%u tc_action=%u",
                   ibuf, p->short_addr, p->status, p->tc_action);

            dm_lock();
            device_record_t *dev = dm_find_by_ieee(ieee);
            if (dev) {
                dm_update_nwk(dev, p->short_addr);
                dm_set_online(dev, true);
            }
            dm_unlock();
            break;
        }

        case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION:
        {
            led_trigger_activity_pulse();
            esp_zb_zdo_signal_leave_indication_params_t *p =
                (esp_zb_zdo_signal_leave_indication_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            char ibuf[20];
            uint64_t ieee = 0;
            memcpy(&ieee, p->device_addr, 8);
            utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
            ZB_LOG("SIGNAL LEAVE_INDICATION ieee=%s rejoin=%u",
                   ibuf, p->rejoin);

            int removed_idx = -1;
            dm_lock();
            device_record_t *dev = dm_find_by_ieee(ieee);
            char leave_name[ZB_EVT_NAME_LEN] = {0};
            if (dev && !p->rejoin) {
                strncpy(leave_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
                if (dm_set_online(dev, false)) {
                    ZB_LOG("DEVICE %s OFFLINE (leave)", dm_display_name(dev));
                }
                removed_idx = dm_index_of(dev);
                if (removed_idx >= 0) {
                    di_forget_device((uint8_t)removed_idx, dev->ieee_addr);
                    zcl_forget_device(dev->ieee_addr);
                    removed_idx = dm_remove(dev);
                }
            }
            dm_unlock();

            if (!p->rejoin) {
                if (removed_idx >= 0) {
                    nvs_cache_delete_device((uint8_t)removed_idx);
                    ZB_LOG("DEVICE %s REMOVED (leave)",
                           leave_name[0] ? leave_name : ibuf);
                }

                zb_event_t evt = {
                    .type   = ZB_EVT_DEVICE_LEAVE,
                    .ieee   = ieee,
                    .online = false,
                };
                strncpy(evt.friendly_name, leave_name, ZB_EVT_NAME_LEN - 1);
                zb_events_emit(&evt);
            }
            break;
        }

        case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        {
            esp_zb_zdo_device_unavailable_params_t *p =
                (esp_zb_zdo_device_unavailable_params_t *)
                    esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            ZB_LOG("SIGNAL DEVICE_UNAVAILABLE nwk=0x%04X", p->short_addr);

            dm_lock();
            device_record_t *dev = dm_find_by_nwk(p->short_addr);
            if (dev && dm_set_online(dev, false)) {
                ZB_LOG("DEVICE %s OFFLINE (unavailable)", dm_display_name(dev));
            }
            dm_unlock();
            break;
        }

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        {
            uint8_t *duration = (uint8_t *)esp_zb_app_signal_get_params(signal_struct->p_app_signal);
            bool open = (*duration > 0);
            ZB_LOG("SIGNAL PERMIT_JOIN %s duration=%u",
                   open ? "OPEN" : "CLOSED", *duration);
            led_set_permit_join(open);

            zb_event_t evt = {
                .type                  = ZB_EVT_PERMIT_JOIN,
                .permit_join_duration  = *duration,
            };
            zb_events_emit(&evt);
            break;
        }

        case ESP_ZB_NLME_STATUS_INDICATION:
            ZB_LOG("SIGNAL NLME_STATUS err=0x%X (network diagnostic)", err);
            break;

        case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
            ZB_LOG("SIGNAL NO_ACTIVE_LINKS_LEFT — network may be isolated");
            break;

        case ESP_ZB_NWK_SIGNAL_PANID_CONFLICT_DETECTED:
            ZB_LOG("SIGNAL PANID_CONFLICT detected");
            break;

        default:
            ZB_LOG("SIGNAL 0x%04X err=0x%X (unhandled)", (unsigned)sig_type, err);
            break;
    }
}

// ---------------------------------------------------------------------------
// Action handler (ZCL) — called from Zigbee task context
// ---------------------------------------------------------------------------

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                    const void *message)
{
    esp_err_t ret = ESP_OK;

    switch (cb_id) {
        case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
            ret = zcl_on_report_attr(
                (const esp_zb_zcl_report_attr_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
            ret = zcl_on_read_attr_resp(
                (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID:
            rc_on_config_resp(
                (const esp_zb_zcl_cmd_config_report_resp_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
            ret = zcl_on_default_resp(
                (const esp_zb_zcl_cmd_default_resp_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID:
            ret = zcl_on_ias_enroll_req(
                (const esp_zb_zcl_ias_zone_enroll_request_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID:
            ret = zcl_on_ias_zone_status(
                (const esp_zb_zcl_ias_zone_status_change_notification_message_t *)
                    message);
            break;

        default:
            // Log unknown ZCL actions at debug level
            ZB_LOG("ZCL action cb_id=0x%04X (unhandled)", (unsigned)cb_id);
            break;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Stack initialisation
// ---------------------------------------------------------------------------

static void esp_zb_task(void *arg)
{
    (void)arg;

    // 1. Platform config (native radio, no host)
    esp_zb_platform_config_t platform_config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_config));

    // 2. Network config
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role          = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy  = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = MAX_CHILDREN,
        },
    };
    ESP_ERROR_CHECK(esp_zb_overall_network_size_set(OVERALL_NETWORK_SIZE));
    esp_zb_init(&zb_nwk_cfg);

    // 3. Create coordinator endpoint
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,  // mains powered
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(
        cluster_list,
        esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(
        cluster_list,
        esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint        = COORD_ENDPOINT,
        .app_profile_id  = HA_PROFILE_ID,
        .app_device_id   = HOME_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
    esp_zb_device_register(ep_list);

    // 4. Register action handler
    esp_zb_core_action_handler_register(zb_action_handler);

    // 5. Set channel mask (all 2.4 GHz channels)
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    // 6. Start stack (autostart = false → we control commissioning via signals)
    ESP_ERROR_CHECK(esp_zb_start(false));

    // 7. Enter main loop (never returns)
    esp_zb_stack_main_loop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void zigbee_core_init(void)
{
    ZB_LOG("Zigbee core: starting stack task");
    // The Zigbee stack requires its own high-priority task
    xTaskCreate(esp_zb_task, "zigbee_main", 8192, NULL, 5, NULL);
}

bool zigbee_core_is_ready(void)
{
    return s_network_ready;
}
