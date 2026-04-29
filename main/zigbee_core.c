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
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_poll_control.h"
#include "zcl/esp_zigbee_zcl_time.h"
#include "zboss_api.h"
#include "zcl/zb_zcl_poll_control.h"
#include "aps/esp_zigbee_aps.h"
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
#define TIME_ATTR_UPDATE_PERIOD_MS 1000u
#define ZIGBEE_TIME_UNIX_EPOCH_DELTA 946684800LL
#define ZIGBEE_TIME_VALID_WINDOW_S 86400u

#define TIME_STATUS_MASTER_BIT          (1u << 0)
#define TIME_STATUS_SYNCHRONIZED_BIT    (1u << 1)
#define POLL_CTRL_FAST_POLL_TIMEOUT_QS  120u

#define ZCL_FRAME_TYPE_PROFILE_WIDE     0x00u
#define ZCL_FRAME_TYPE_CLUSTER_SPECIFIC 0x01u
#define ZCL_FRAME_TYPE_MASK             0x03u
#define ZCL_FRAME_MANUF_SPECIFIC_BIT    (1u << 2)
#define ZCL_FRAME_DIRECTION_BIT         (1u << 3)
#define ZCL_FRAME_DISABLE_DEFAULT_RSP   (1u << 4)

#define ZCL_CMD_READ_ATTR               0x00u
#define ZCL_CMD_WRITE_ATTR              0x02u
#define ZCL_CMD_CONFIG_REPORT           0x06u
#define ZCL_CMD_READ_REPORT_CFG         0x08u
#define ZCL_CMD_REPORT_ATTR             0x0Au
#define ZCL_CMD_DEFAULT_RSP             0x0Bu
#define ZCL_CMD_DISCOVER_ATTR           0x0Cu

static volatile bool s_network_ready = false;
static bool s_time_attr_refresh_started = false;

static esp_zb_time_cluster_cfg_t s_time_cluster_cfg = {
    .time = ESP_ZB_ZCL_TIME_TIME_DEFAULT_VALUE,
    .time_status = ESP_ZB_ZCL_TIME_TIME_STATUS_DEFAULT_VALUE,
};
static int32_t s_time_zone = ESP_ZB_ZCL_TIME_TIME_ZONE_DEFAULT_VALUE;
static uint32_t s_dst_start = ESP_ZB_ZCL_TIME_DST_START_DEFAULT_VALUE;
static uint32_t s_dst_end = ESP_ZB_ZCL_TIME_DST_END_DEFAULT_VALUE;
static uint32_t s_dst_shift = ESP_ZB_ZCL_TIME_DST_SHIFT_DEFAULT_VALUE;
static uint32_t s_standard_time = ESP_ZB_ZCL_TIME_STANDARD_TIME_DEFAULT_VALUE;
static uint32_t s_local_time = ESP_ZB_ZCL_TIME_LOCAL_TIME_DEFAULT_VALUE;
static uint32_t s_last_set_time = ESP_ZB_ZCL_TIME_LAST_SET_TIME_DEFAULT_VALUE;
static uint32_t s_valid_until_time = ESP_ZB_ZCL_TIME_VALID_UNTIL_TIME_DEFAULT_VALUE;
static esp_zb_poll_control_cluster_cfg_t s_poll_control_cluster_cfg = {
    .check_in_interval = ESP_ZB_ZCL_POLL_CONTROL_CHECK_IN_INTERVAL_DEFAULT_VALUE,
    .long_poll_interval = ESP_ZB_ZCL_POLL_CONTROL_LONG_POLL_INTERVAL_DEFAULT_VALUE,
    .short_poll_interval = ESP_ZB_ZCL_POLL_CONTROL_SHORT_POLL_INTERVAL_DEFAULT_VALUE,
    .fast_poll_timeout = ESP_ZB_ZCL_POLL_CONTROL_FAST_POLL_TIMEOUT_DEFAULT_VALUE,
    .check_in_interval_min = ESP_ZB_ZCL_POLL_CONTROL_MIN_CHECK_IN_INTERVAL_DEFAULT_VALUE,
    .long_poll_interval_min = ESP_ZB_ZCL_POLL_CONTROL_LONG_POLL_MIN_INTERVAL_DEFAULT_VALUE,
    .fast_poll_timeout_max = ESP_ZB_ZCL_POLL_CONTROL_FAST_POLL_MAX_TIMEOUT_DEFAULT_VALUE,
};

typedef struct {
    esp_zb_zcl_cmd_info_t info;
} zb_cmd_info_message_t;

static const char *zb_action_name(esp_zb_core_action_callback_id_t cb_id)
{
    switch (cb_id) {
        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
            return "READ_ATTR_RESP";
        case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID:
            return "WRITE_ATTR_RESP";
        case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID:
            return "REPORT_CONFIG_RESP";
        case ESP_ZB_CORE_CMD_READ_REPORT_CFG_RESP_CB_ID:
            return "READ_REPORT_CFG_RESP";
        case ESP_ZB_CORE_CMD_DISC_ATTR_RESP_CB_ID:
            return "DISC_ATTR_RESP";
        case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
            return "DEFAULT_RESP";
        case ESP_ZB_CORE_CMD_OPERATE_GROUP_RESP_CB_ID:
            return "GROUP_OPERATE_RESP";
        case ESP_ZB_CORE_CMD_VIEW_GROUP_RESP_CB_ID:
            return "GROUP_VIEW_RESP";
        case ESP_ZB_CORE_CMD_GET_GROUP_MEMBERSHIP_RESP_CB_ID:
            return "GROUP_MEMBERSHIP_RESP";
        case ESP_ZB_CORE_CMD_OPERATE_SCENE_RESP_CB_ID:
            return "SCENE_OPERATE_RESP";
        case ESP_ZB_CORE_CMD_VIEW_SCENE_RESP_CB_ID:
            return "SCENE_VIEW_RESP";
        case ESP_ZB_CORE_CMD_GET_SCENE_MEMBERSHIP_RESP_CB_ID:
            return "SCENE_MEMBERSHIP_RESP";
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID:
            return "IAS_ENROLL_REQ";
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID:
            return "IAS_ZONE_STATUS_NOT";
        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
            return "CUSTOM_CLUSTER_REQ";
        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_RESP_CB_ID:
            return "CUSTOM_CLUSTER_RESP";
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID:
            return "PRIVILEGE_REQ";
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_RESP_CB_ID:
            return "PRIVILEGE_RESP";
        case ESP_ZB_CORE_CMD_TOUCHLINK_GET_GROUP_ID_RESP_CB_ID:
            return "TOUCHLINK_GROUP_ID_RESP";
        case ESP_ZB_CORE_CMD_TOUCHLINK_GET_ENDPOINT_LIST_RESP_CB_ID:
            return "TOUCHLINK_ENDPOINT_LIST_RESP";
        case ESP_ZB_CORE_CMD_THERMOSTAT_GET_WEEKLY_SCHEDULE_RESP_CB_ID:
            return "THERMOSTAT_WEEKLY_SCHED_RESP";
        case ESP_ZB_CORE_CMD_GREEN_POWER_RECV_CB_ID:
            return "GREEN_POWER_RECV";
        case ESP_ZB_CORE_POLL_CONTROL_CHECK_IN_REQ_CB_ID:
            return "POLL_CONTROL_CHECK_IN_REQ";
        case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
            return "REPORT_ATTR";
        default:
            return "UNKNOWN";
    }
}

static bool zb_action_has_cmd_info(esp_zb_core_action_callback_id_t cb_id)
{
    switch (cb_id) {
        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_READ_REPORT_CFG_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_DISC_ATTR_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_OPERATE_GROUP_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_VIEW_GROUP_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_GET_GROUP_MEMBERSHIP_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_OPERATE_SCENE_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_VIEW_SCENE_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_GET_SCENE_MEMBERSHIP_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID:
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID:
        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID:
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_TOUCHLINK_GET_GROUP_ID_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_TOUCHLINK_GET_ENDPOINT_LIST_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_THERMOSTAT_GET_WEEKLY_SCHEDULE_RESP_CB_ID:
        case ESP_ZB_CORE_CMD_GREEN_POWER_RECV_CB_ID:
            return true;
        default:
            return false;
    }
}

static void format_hex_preview(const uint8_t *data, size_t len,
                               char *buf, size_t buf_len)
{
    size_t used = 0;

    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';

    if (!data || len == 0) {
        snprintf(buf, buf_len, "-");
        return;
    }

    size_t preview_len = len > 12 ? 12 : len;
    for (size_t i = 0; i < preview_len; i++) {
        int n = snprintf(buf + used, buf_len - used, "%s%02X",
                         i ? " " : "", data[i]);
        if (n < 0 || (size_t)n >= buf_len - used) {
            break;
        }
        used += (size_t)n;
    }
    if (len > preview_len && used < buf_len - 4) {
        snprintf(buf + used, buf_len - used, " ...");
    }
}

static const char *zcl_profile_cmd_name(uint8_t cmd_id)
{
    switch (cmd_id) {
        case ZCL_CMD_READ_ATTR:
            return "READ_ATTR";
        case ZCL_CMD_WRITE_ATTR:
            return "WRITE_ATTR";
        case ZCL_CMD_CONFIG_REPORT:
            return "CONFIG_REPORT";
        case ZCL_CMD_READ_REPORT_CFG:
            return "READ_REPORT_CFG";
        case ZCL_CMD_REPORT_ATTR:
            return "REPORT_ATTR";
        case ZCL_CMD_DEFAULT_RSP:
            return "DEFAULT_RSP";
        case ZCL_CMD_DISCOVER_ATTR:
            return "DISCOVER_ATTR";
        default:
            return "PROFILE_CMD";
    }
}

static bool zcl_profile_cmd_is_request(uint8_t cmd_id)
{
    switch (cmd_id) {
        case ZCL_CMD_READ_ATTR:
        case ZCL_CMD_WRITE_ATTR:
        case ZCL_CMD_CONFIG_REPORT:
        case ZCL_CMD_READ_REPORT_CFG:
        case ZCL_CMD_DISCOVER_ATTR:
            return true;
        default:
            return false;
    }
}

static void format_zcl_attr_id_list(const uint8_t *payload, size_t len,
                                    char *buf, size_t buf_len)
{
    size_t used = 0;

    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';

    if (!payload || len < 2) {
        snprintf(buf, buf_len, "-");
        return;
    }

    size_t attr_count = len / 2u;
    if (attr_count > 8u) {
        attr_count = 8u;
    }

    for (size_t i = 0; i < attr_count; i++) {
        uint16_t attr_id = (uint16_t)payload[i * 2u] |
                           ((uint16_t)payload[i * 2u + 1u] << 8);
        int n = snprintf(buf + used, buf_len - used, "%s0x%04X",
                         i ? "," : "", attr_id);
        if (n < 0 || (size_t)n >= buf_len - used) {
            break;
        }
        used += (size_t)n;
    }

    if ((len / 2u) > attr_count && used < buf_len - 4) {
        snprintf(buf + used, buf_len - used, ",...");
    }
}

static void log_zcl_frame_preview(const esp_zb_apsde_data_ind_t *ind,
                                  const device_record_t *dev)
{
    if (!ind || !dev || ind->profile_id != HA_PROFILE_ID ||
        ind->dst_endpoint != COORD_ENDPOINT || !ind->asdu ||
        ind->asdu_length < 3) {
        return;
    }

    const uint8_t *asdu = ind->asdu;
    size_t len = (size_t)ind->asdu_length;
    uint8_t frame_control = asdu[0];
    uint8_t frame_type = frame_control & ZCL_FRAME_TYPE_MASK;
    bool manuf_specific =
        (frame_control & ZCL_FRAME_MANUF_SPECIFIC_BIT) != 0;
    bool server_to_client =
        (frame_control & ZCL_FRAME_DIRECTION_BIT) != 0;
    bool disable_default_rsp =
        (frame_control & ZCL_FRAME_DISABLE_DEFAULT_RSP) != 0;
    size_t pos = 1;
    uint16_t manuf_code = 0;

    if (manuf_specific) {
        if (len < 5) {
            return;
        }
        manuf_code = (uint16_t)asdu[pos] | ((uint16_t)asdu[pos + 1] << 8);
        pos += 2;
    }

    uint8_t tsn = asdu[pos++];
    uint8_t cmd_id = asdu[pos++];
    const uint8_t *payload = asdu + pos;
    size_t payload_len = len - pos;

    if (frame_type == ZCL_FRAME_TYPE_PROFILE_WIDE) {
        if (!zcl_profile_cmd_is_request(cmd_id)) {
            return;
        }

        char attrs[96];
        attrs[0] = '\0';

        if (cmd_id == ZCL_CMD_READ_ATTR) {
            format_zcl_attr_id_list(payload, payload_len, attrs, sizeof(attrs));
        }

        ZB_LOG("RX ZCL_FRAME src=%s/%u dst_ep=%u cluster=%s cmd=%s(0x%02X) dir=%s tsn=0x%02X disable_default=%u mfr_specific=%u mfr=0x%04X payload_len=%lu",
               dm_display_name(dev), ind->src_endpoint, ind->dst_endpoint,
               utils_cluster_name(ind->cluster_id), zcl_profile_cmd_name(cmd_id),
               cmd_id, server_to_client ? "srv_to_cli" : "cli_to_srv", tsn,
               disable_default_rsp ? 1u : 0u, manuf_specific ? 1u : 0u,
               manuf_code, (unsigned long)payload_len);
        if (attrs[0]) {
            ZB_LOG("RX ZCL_READ_ATTRS src=%s/%u cluster=%s tsn=0x%02X attrs=[%s]",
                   dm_display_name(dev), ind->src_endpoint,
                   utils_cluster_name(ind->cluster_id), tsn, attrs);
        }
    } else if (frame_type == ZCL_FRAME_TYPE_CLUSTER_SPECIFIC) {
        ZB_LOG("RX ZCL_FRAME src=%s/%u dst_ep=%u cluster=%s cmd=CLUSTER_CMD(0x%02X) dir=%s tsn=0x%02X disable_default=%u mfr_specific=%u mfr=0x%04X payload_len=%lu",
               dm_display_name(dev), ind->src_endpoint, ind->dst_endpoint,
               utils_cluster_name(ind->cluster_id), cmd_id,
               server_to_client ? "srv_to_cli" : "cli_to_srv", tsn,
               disable_default_rsp ? 1u : 0u, manuf_specific ? 1u : 0u,
               manuf_code, (unsigned long)payload_len);
    } else {
        ZB_LOG("RX ZCL_FRAME src=%s/%u dst_ep=%u cluster=%s frame_type=0x%02X cmd=0x%02X dir=%s tsn=0x%02X payload_len=%lu",
               dm_display_name(dev), ind->src_endpoint, ind->dst_endpoint,
               utils_cluster_name(ind->cluster_id), frame_type, cmd_id,
               server_to_client ? "srv_to_cli" : "cli_to_srv", tsn,
               (unsigned long)payload_len);
    }
}

static void log_generic_zcl_message(esp_zb_core_action_callback_id_t cb_id,
                                    const void *message)
{
    if (!message || !zb_action_has_cmd_info(cb_id)) {
        return;
    }

    const zb_cmd_info_message_t *msg = (const zb_cmd_info_message_t *)message;
    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    if (!dev) {
        return;
    }

    ZB_LOG("RX ZCL src=%s/%u cb=%s(0x%04X) cluster=%s cmd=0x%02X common=%u dir=%u tsn=0x%02X status=0x%02X rssi=%d",
           dm_display_name(dev), msg->info.src_endpoint,
           zb_action_name(cb_id), (unsigned)cb_id,
           utils_cluster_name(msg->info.cluster),
           msg->info.command.id, msg->info.command.is_common,
           msg->info.command.direction, msg->info.header.tsn,
           msg->info.status, msg->info.header.rssi);
}

static void touch_generic_zcl_source(esp_zb_core_action_callback_id_t cb_id,
                                     const void *message)
{
    if (!message || !zb_action_has_cmd_info(cb_id)) {
        return;
    }

    const zb_cmd_info_message_t *msg = (const zb_cmd_info_message_t *)message;
    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    if (!dev) {
        return;
    }

    int8_t rssi = msg->info.header.rssi;
    if (rssi != 0) {
        dm_touch(dev, esp_zb_rssi_to_lqi(rssi), rssi);
    } else {
        dm_touch(dev, 0, 0);
    }
}

static bool zb_aps_data_indication_handler(esp_zb_apsde_data_ind_t ind)
{
    device_record_t *dev = dm_find_by_nwk(ind.src_short_addr);
    if (!dev) {
        return false;
    }

    dm_touch(dev, 0, 0);

    char asdu_preview[64];
    format_hex_preview(ind.asdu, (size_t)ind.asdu_length,
                       asdu_preview, sizeof(asdu_preview));

    ZB_LOG("APS RX src=%s/%u dst=0x%04X/%u profile=0x%04X cluster=%s len=%lu status=0x%02X sec=0x%02X lqi=%d asdu=%s",
           dm_display_name(dev), ind.src_endpoint,
           ind.dst_short_addr, ind.dst_endpoint,
           ind.profile_id, utils_cluster_name(ind.cluster_id),
           (unsigned long)ind.asdu_length, ind.status,
           ind.security_status, ind.lqi, asdu_preview);
    log_zcl_frame_preview(&ind, dev);

    return false;
}

static bool zigbee_time_from_unix(time_t unix_time, uint32_t *out_zigbee_time)
{
    if (!out_zigbee_time || unix_time < (time_t)ZIGBEE_TIME_UNIX_EPOCH_DELTA) {
        return false;
    }

    uint64_t zigbee_time =
        (uint64_t)((int64_t)unix_time - ZIGBEE_TIME_UNIX_EPOCH_DELTA);
    if (zigbee_time >= 0xFFFFFFFFULL) {
        return false;
    }

    *out_zigbee_time = (uint32_t)zigbee_time;
    return true;
}

static int32_t local_utc_offset_seconds(time_t now)
{
    struct tm local_tm = {0};
    struct tm utc_tm = {0};

    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);

    local_tm.tm_isdst = -1;
    utc_tm.tm_isdst = 0;

    time_t local_epoch = mktime(&local_tm);
    time_t utc_epoch = mktime(&utc_tm);
    return (int32_t)difftime(local_epoch, utc_epoch);
}

static void time_attr_update_alarm(uint8_t param)
{
    (void)param;

    uint32_t zigbee_time = ESP_ZB_ZCL_TIME_TIME_DEFAULT_VALUE;
    uint32_t standard_time = ESP_ZB_ZCL_TIME_STANDARD_TIME_DEFAULT_VALUE;
    uint32_t local_time = ESP_ZB_ZCL_TIME_LOCAL_TIME_DEFAULT_VALUE;
    uint32_t last_set_time = ESP_ZB_ZCL_TIME_LAST_SET_TIME_DEFAULT_VALUE;
    uint32_t valid_until_time = ESP_ZB_ZCL_TIME_VALID_UNTIL_TIME_DEFAULT_VALUE;
    uint8_t time_status = 0;
    int32_t time_zone = 0;

    if (utils_wall_time_valid()) {
        time_t now = time(NULL);
        if (zigbee_time_from_unix(now, &zigbee_time)) {
            time_zone = local_utc_offset_seconds(now);

            int64_t base_local_time = (int64_t)zigbee_time + time_zone;
            if (base_local_time >= 0 && base_local_time < 0xFFFFFFFFLL) {
                standard_time = (uint32_t)base_local_time;
                local_time = (uint32_t)base_local_time;
            }

            last_set_time = zigbee_time;
            if (zigbee_time <= (UINT32_MAX - ZIGBEE_TIME_VALID_WINDOW_S)) {
                valid_until_time = zigbee_time + ZIGBEE_TIME_VALID_WINDOW_S;
            }

            time_status = TIME_STATUS_MASTER_BIT |
                          TIME_STATUS_SYNCHRONIZED_BIT;
        }
    }

    s_time_cluster_cfg.time = zigbee_time;
    s_time_cluster_cfg.time_status = time_status;
    s_time_zone = time_zone;
    s_dst_start = ESP_ZB_ZCL_TIME_DST_START_DEFAULT_VALUE;
    s_dst_end = ESP_ZB_ZCL_TIME_DST_END_DEFAULT_VALUE;
    s_dst_shift = 0;
    s_standard_time = standard_time;
    s_local_time = local_time;
    s_last_set_time = last_set_time;
    s_valid_until_time = valid_until_time;

    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_TIME_ID,
                                 &s_time_cluster_cfg.time, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_TIME_STATUS_ID,
                                 &s_time_cluster_cfg.time_status, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID,
                                 &s_time_zone, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_START_ID,
                                 &s_dst_start, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_END_ID,
                                 &s_dst_end, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_SHIFT_ID,
                                 &s_dst_shift, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_STANDARD_TIME_ID,
                                 &s_standard_time, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_LOCAL_TIME_ID,
                                 &s_local_time, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_LAST_SET_TIME_ID,
                                 &s_last_set_time, false);
    esp_zb_zcl_set_attribute_val(COORD_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TIME,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_TIME_VALID_UNTIL_TIME_ID,
                                 &s_valid_until_time, false);

    esp_zb_scheduler_alarm(time_attr_update_alarm, 0, TIME_ATTR_UPDATE_PERIOD_MS);
}

static void start_time_attr_refresh(void)
{
    if (s_time_attr_refresh_started) {
        return;
    }

    s_time_attr_refresh_started = true;
    esp_zb_scheduler_alarm(time_attr_update_alarm, 0, 0);
}

static uint16_t poll_control_fast_poll_timeout(uint16_t device_timeout_qs)
{
    if (device_timeout_qs == 0 || device_timeout_qs > POLL_CTRL_FAST_POLL_TIMEOUT_QS) {
        return POLL_CTRL_FAST_POLL_TIMEOUT_QS;
    }
    return device_timeout_qs;
}

static void send_poll_control_check_in_response(uint16_t nwk_addr,
                                                uint8_t endpoint,
                                                bool start_fast_poll,
                                                uint16_t timeout_qs)
{
    zb_bufid_t buf = zb_buf_get_out();
    if (buf == ZB_BUF_INVALID) {
        ZB_LOG("POLL_CTRL CHECK_IN_RSP dst=0x%04X/%u no buffer",
               nwk_addr, endpoint);
        return;
    }

    ZB_LOG("POLL_CTRL CHECK_IN_RSP dst=0x%04X/%u start=%u timeout_qs=%u",
           nwk_addr, endpoint, start_fast_poll ? 1u : 0u, timeout_qs);
    ZB_ZCL_POLL_CONTROL_SEND_CHECK_IN_RES(
        buf,
        nwk_addr,
        ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        endpoint,
        COORD_ENDPOINT,
        HA_PROFILE_ID,
        ZB_TRUE,
        NULL,
        start_fast_poll ? ZB_TRUE : ZB_FALSE,
        timeout_qs);
}

static esp_err_t zcl_on_poll_control_check_in_req(
    const esp_zb_zcl_poll_control_check_in_req_message_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }

    device_record_t *dev = dm_find_by_nwk(msg->src_short_addr);
    if (dev) {
        ZB_LOG("POLL_CTRL CHECK_IN src=%s/%u timeout_qs=%u status=0x%02X",
               dm_display_name(dev), msg->src_ep_id,
               msg->fast_poll_timeout, msg->info.status);
        dm_touch(dev, 0, 0);
    } else {
        ZB_LOG("POLL_CTRL CHECK_IN src=0x%04X/%u timeout_qs=%u status=0x%02X",
               msg->src_short_addr, msg->src_ep_id,
               msg->fast_poll_timeout, msg->info.status);
    }

    bool start_fast_poll = rc_device_has_reporting_pending(dev);
    uint16_t timeout_qs = start_fast_poll
        ? poll_control_fast_poll_timeout(msg->fast_poll_timeout)
        : 0;
    send_poll_control_check_in_response(msg->src_short_addr, msg->src_ep_id,
                                        start_fast_poll, timeout_qs);

    if (start_fast_poll) {
        rc_configure_pending_sleepy_now(dev, "poll_check_in");
    }

    return ESP_OK;
}

static bool device_can_configure_reporting_now(const device_record_t *dev)
{
    return dev &&
           !dev->reporting_configured &&
           !dev->report_cfg_in_progress &&
           dm_has_complete_descriptors(dev) &&
           dev->state >= DEV_STATE_INTERVIEWED;
}

static void configure_reporting_from_awake_window(device_record_t *dev,
                                                  const char *reason)
{
    if (!device_can_configure_reporting_now(dev)) {
        return;
    }

    ZB_LOG("REPORT_CFG awake window for %s reason=%s: immediate configure",
           dm_display_name(dev), reason ? reason : "?");
    rc_configure_device(dev);
}

// ---------------------------------------------------------------------------
// Maintenance alarm — fires every MAINTENANCE_PERIOD_MS
// ---------------------------------------------------------------------------

static void maintenance_alarm(uint8_t param)
{
    (void)param;
    nvs_cache_save_dirty();
    rc_check_reporting_timeouts();
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
                start_time_attr_refresh();
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
                start_time_attr_refresh();
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
                    (!dm_has_complete_descriptors(dev) ||
                     dev->state == DEV_STATE_NEW ||
                     dev->state == DEV_STATE_FAILED);

                dm_unlock();

                if (needs_interview) {
                    di_enqueue(dev);
                } else {
                    ZB_LOG("DEVICE %s rejoined (already known, state=%s)",
                           dm_display_name(dev),
                           utils_device_state_name((int)dev->state));
                    if (device_can_configure_reporting_now(dev)) {
                        configure_reporting_from_awake_window(dev, "device_annce");
                    } else if (!dev->reporting_configured) {
                        di_enqueue(dev);
                    }
                }

                // Notify registered event consumers
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
            if (dev && (!dm_has_complete_descriptors(dev) ||
                        dev->state == DEV_STATE_NEW ||
                        dev->state == DEV_STATE_FAILED)) {
                di_enqueue(dev);
            } else {
                configure_reporting_from_awake_window(dev, "device_update");
            }
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
            if (dev) {
                bool fresh_sleepy = false;
                if (dev->is_sleepy && dev->last_seen_ms != 0) {
                    uint32_t age_ms = utils_uptime_ms() - dev->last_seen_ms;
                    fresh_sleepy = age_ms <= rc_presence_offline_timeout_ms(true);
                    if (fresh_sleepy) {
                        ZB_LOG("DEVICE %s unavailable ignored (sleepy, last_seen=%lu s ago)",
                               dm_display_name(dev),
                               (unsigned long)(age_ms / 1000u));
                    }
                }
                if (!fresh_sleepy) {
                    uint32_t age_ms = dev->last_seen_ms != 0
                        ? utils_uptime_ms() - dev->last_seen_ms
                        : 0xFFFFFFFFu;
                    if (!dev->is_sleepy && dev->online &&
                        age_ms <= rc_presence_offline_timeout_ms(false)) {
                        if (dm_request_presence_probe(dev, "unavailable")) {
                            ZB_LOG("DEVICE %s unavailable pending confirmation (last_seen=%lu s ago)",
                                   dm_display_name(dev),
                                   (unsigned long)(age_ms / 1000u));
                        }
                    } else if (dm_set_online(dev, false)) {
                        ZB_LOG("DEVICE %s OFFLINE (unavailable)", dm_display_name(dev));
                    }
                }
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

        case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
            if (err == ESP_OK) {
                ZB_LOG("SIGNAL PRODUCTION_CONFIG_READY OK");
            } else {
                ZB_LOG("SIGNAL PRODUCTION_CONFIG_READY unavailable err=0x%X",
                       err);
            }
            break;

        default:
            ZB_LOG("SIGNAL 0x%04X err=0x%X (unhandled)", (unsigned)sig_type, err);
            break;
    }
}

// ---------------------------------------------------------------------------
// Action handler (ZCL) — called from Zigbee task context
// ---------------------------------------------------------------------------

static void zb_zcl_send_status_handler(esp_zb_zcl_command_send_status_message_t message)
{
    if (message.dst_addr.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
        ZB_LOG("TX STATUS status=0x%X tsn=0x%02X dst=0x%04X src_ep=%u dst_ep=%u",
               (unsigned)message.status, message.tsn, message.dst_addr.u.short_addr,
               message.src_endpoint, message.dst_endpoint);
    } else {
        ZB_LOG("TX STATUS status=0x%X tsn=0x%02X addr_type=0x%02X src_ep=%u dst_ep=%u",
               (unsigned)message.status, message.tsn, message.dst_addr.addr_type,
               message.src_endpoint, message.dst_endpoint);
    }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                    const void *message)
{
    esp_err_t ret = ESP_OK;

    log_generic_zcl_message(cb_id, message);
    touch_generic_zcl_source(cb_id, message);

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

        case ESP_ZB_CORE_CMD_READ_REPORT_CFG_RESP_CB_ID:
            rc_on_read_report_cfg_resp(
                (const esp_zb_zcl_cmd_read_report_config_resp_message_t *)message);
            break;

        case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID:
            rc_on_write_attr_resp(
                (const esp_zb_zcl_cmd_write_attr_resp_message_t *)message);
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

        case ESP_ZB_CORE_POLL_CONTROL_CHECK_IN_REQ_CB_ID:
            ret = zcl_on_poll_control_check_in_req(
                (const esp_zb_zcl_poll_control_check_in_req_message_t *)message);
            break;

        default:
            ZB_LOG("ZCL action cb=%s(0x%04X) (unhandled)",
                   zb_action_name(cb_id), (unsigned)cb_id);
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
    esp_zb_attribute_list_t *time_cluster =
        esp_zb_time_cluster_create(&s_time_cluster_cfg);
    esp_zb_attribute_list_t *poll_control_cluster =
        esp_zb_poll_control_cluster_create(&s_poll_control_cluster_cfg);

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(
        cluster_list,
        esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(
        cluster_list,
        esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID,
                                 &s_time_zone);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_START_ID,
                                 &s_dst_start);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_END_ID,
                                 &s_dst_end);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_DST_SHIFT_ID,
                                 &s_dst_shift);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_STANDARD_TIME_ID,
                                 &s_standard_time);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_LOCAL_TIME_ID,
                                 &s_local_time);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_LAST_SET_TIME_ID,
                                 &s_last_set_time);
    esp_zb_time_cluster_add_attr(time_cluster,
                                 ESP_ZB_ZCL_ATTR_TIME_VALID_UNTIL_TIME_ID,
                                 &s_valid_until_time);
    esp_zb_cluster_list_add_time_cluster(cluster_list, time_cluster,
                                         ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_poll_control_cluster(
        cluster_list, poll_control_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

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
    esp_zb_aps_data_indication_handler_register(zb_aps_data_indication_handler);
    esp_zb_zcl_command_send_status_handler_register(zb_zcl_send_status_handler);

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
