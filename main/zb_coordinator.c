#include "zb_coordinator.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"
#include "aps/esp_zigbee_aps.h"
#include "esp_zigbee_endpoint.h"
#include "esp_zigbee_cluster.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_occupancy_sensing.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "timebase.h"
#include "zb_trace.h"

static const char *TAG = "zb_coord";
static zb_persist_state_t s_runtime;
static zb_coordinator_event_cb_t s_event_cb = NULL;
static bool s_stack_started = false;
static const uint8_t COORDINATOR_ENDPOINT = 1;
static void emit_event(const char *name);

#define IDENTITY_JOB_MAX 16
#define IDENTITY_RETRY_MAX 4

typedef struct {
    bool used;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t attempts;
    bool got_manufacturer;
    bool got_model;
    TickType_t next_try_tick;
} identity_job_t;

static identity_job_t s_identity_jobs[IDENTITY_JOB_MAX];

static bool has_cluster(const uint16_t *clusters, size_t clusters_len, uint16_t cluster_id)
{
    if (clusters == NULL) {
        return false;
    }
    for (size_t i = 0; i < clusters_len; ++i) {
        if (clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static identity_job_t *identity_job_find(uint16_t short_addr, uint8_t endpoint)
{
    for (size_t i = 0; i < IDENTITY_JOB_MAX; ++i) {
        if (s_identity_jobs[i].used && s_identity_jobs[i].short_addr == short_addr && s_identity_jobs[i].endpoint == endpoint) {
            return &s_identity_jobs[i];
        }
    }
    return NULL;
}

static identity_job_t *identity_job_get_or_create(uint16_t short_addr, uint8_t endpoint)
{
    identity_job_t *j = identity_job_find(short_addr, endpoint);
    if (j != NULL) {
        return j;
    }
    for (size_t i = 0; i < IDENTITY_JOB_MAX; ++i) {
        if (!s_identity_jobs[i].used) {
            memset(&s_identity_jobs[i], 0, sizeof(s_identity_jobs[i]));
            s_identity_jobs[i].used = true;
            s_identity_jobs[i].short_addr = short_addr;
            s_identity_jobs[i].endpoint = endpoint;
            return &s_identity_jobs[i];
        }
    }
    return NULL;
}

static void identity_job_schedule(uint16_t short_addr, uint8_t endpoint, TickType_t delay_ticks)
{
    identity_job_t *j = identity_job_get_or_create(short_addr, endpoint);
    if (j == NULL) {
        ESP_LOGW(TAG, "[T+%07.3f] Sin slot para entrevista identidad short=0x%04X ep=%u", timebase_now_s(), short_addr, endpoint);
        return;
    }
    j->next_try_tick = xTaskGetTickCount() + delay_ticks;
    j->attempts = 0;
    j->got_manufacturer = false;
    j->got_model = false;
}

static void attr_to_text(const uint8_t *value, uint16_t value_size, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (value == NULL || value_size == 0) {
        return;
    }
    const size_t n = (size_t)value[0];
    if (n == 0 || n > (size_t)(value_size - 1U)) {
        return;
    }
    size_t copy_n = n;
    if (copy_n >= out_len) {
        copy_n = out_len - 1U;
    }
    memcpy(out, &value[1], copy_n);
    out[copy_n] = '\0';
}

static void zcl_read_attr_req(uint16_t dst_short, uint8_t dst_ep, uint16_t cluster_id, uint16_t attr_id)
{
    uint16_t attr_field[] = {attr_id};
    esp_zb_zcl_read_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = dst_short;
    cmd.zcl_basic_cmd.dst_endpoint = dst_ep;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = cluster_id;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.attr_number = 1;
    cmd.attr_field = attr_field;
    const uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&cmd);
    ESP_LOGI(TAG, "[T+%07.3f] ReadAttr REQ short=0x%04X ep=%u cl=0x%04X attr=0x%04X tsn=0x%02X", timebase_now_s(), dst_short, dst_ep,
             cluster_id, attr_id, tsn);
}

static void zcl_config_report_req(uint16_t dst_short, uint8_t dst_ep, uint16_t cluster_id, uint16_t attr_id, uint8_t attr_type, uint16_t min_s,
                                  uint16_t max_s, const void *reportable_change)
{
    esp_zb_zcl_config_report_record_t rec = {0};
    rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    rec.attributeID = attr_id;
    rec.attrType = attr_type;
    rec.min_interval = min_s;
    rec.max_interval = max_s;
    rec.reportable_change = (void *)reportable_change;

    esp_zb_zcl_config_report_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = dst_short;
    cmd.zcl_basic_cmd.dst_endpoint = dst_ep;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = cluster_id;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.record_number = 1;
    cmd.record_field = &rec;
    const uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
    ESP_LOGI(TAG, "[T+%07.3f] ConfigReport REQ short=0x%04X ep=%u cl=0x%04X attr=0x%04X tsn=0x%02X", timebase_now_s(), dst_short, dst_ep,
             cluster_id, attr_id, tsn);
}

static esp_err_t register_coordinator_endpoint(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    if (ep_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    if (cluster_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_UNKNOWN,
    };
    esp_zb_attribute_list_t *basic_attr = esp_zb_basic_cluster_create(&basic_cfg);
    if (basic_attr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = COORDINATOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg));
    return esp_zb_device_register(ep_list);
}

static void interview_request_simple_desc(uint16_t short_addr, uint8_t endpoint);
static void node_desc_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, esp_zb_af_node_desc_t *node_desc, void *user_ctx);

static void interview_request_node_desc(uint16_t short_addr)
{
    esp_zb_zdo_node_desc_req_param_t req = {
        .dst_nwk_addr = short_addr,
    };
    esp_zb_zdo_node_desc_req(&req, node_desc_cb, (void *)(uintptr_t)short_addr);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    const uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL) {
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista simple desc fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
        return;
    }

    const uint8_t in_count = simple_desc->app_input_cluster_count;
    const uint8_t out_count = simple_desc->app_output_cluster_count;
    const uint8_t total = (uint8_t)(in_count + out_count);
    uint16_t clusters_in[DEVICE_TABLE_MAX_CLUSTERS] = {0};
    uint16_t clusters_out[DEVICE_TABLE_MAX_CLUSTERS] = {0};

    size_t in_len = 0;
    size_t out_len = 0;
    for (uint8_t i = 0; i < in_count && i < DEVICE_TABLE_MAX_CLUSTERS; ++i) {
        clusters_in[in_len++] = simple_desc->app_cluster_list[i];
    }
    for (uint8_t i = 0; i < out_count && (i + in_count) < total && i < DEVICE_TABLE_MAX_CLUSTERS; ++i) {
        clusters_out[out_len++] = simple_desc->app_cluster_list[in_count + i];
    }

    device_table_update_simple_desc(short_addr, simple_desc->endpoint, simple_desc->app_device_id, clusters_in, in_len, clusters_out, out_len);
    ESP_LOGI(TAG, "[T+%07.3f] Entrevista EP=%u short=0x%04X device_id=0x%04X in=%u out=%u", timebase_now_s(), simple_desc->endpoint, short_addr,
             simple_desc->app_device_id, in_count, out_count);
    emit_event("INTERVIEW_SIMPLE_DESC");

    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_BASIC)) {
        /* La lectura de identidad se difiere para evitar la ventana previa a "Device Authorized". */
        identity_job_schedule(short_addr, simple_desc->endpoint, pdMS_TO_TICKS(1200));
    }

    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT)) {
        static const int16_t temp_delta = 50; /* 0.50 C */
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                          ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                              ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, 30, 300, &temp_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Temp sensor detectado short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT)) {
        static const uint16_t hum_delta = 100; /* 1.00 % */
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                          ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                              ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_U16, 30, 300, &hum_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Humidity sensor detectado short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING)) {
        static const uint8_t occ_delta = 1;
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                          ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                              ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, ESP_ZB_ZCL_ATTR_TYPE_U8, 1, 120, &occ_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Presence sensor detectado short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)) {
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
        ESP_LOGI(TAG,
                 "[T+%07.3f] Switch/actuador detectado short=0x%04X ep=%u (comandos: ON=0x%02X OFF=0x%02X TOGGLE=0x%02X)",
                 timebase_now_s(), short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
                 ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID);
    }
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    const uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_id_list == NULL || ep_count == 0) {
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista active_ep fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
        return;
    }

    ESP_LOGI(TAG, "[T+%07.3f] Entrevista short=0x%04X endpoints=%u", timebase_now_s(), short_addr, ep_count);
    for (uint8_t i = 0; i < ep_count; ++i) {
        interview_request_simple_desc(short_addr, ep_id_list[i]);
    }
}

static void node_desc_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, esp_zb_af_node_desc_t *node_desc, void *user_ctx)
{
    (void)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || node_desc == NULL) {
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista node_desc fallo short=0x%04X status=0x%02X", timebase_now_s(), addr, zdo_status);
        return;
    }
    device_table_update_node_desc(addr, node_desc->manufacturer_code, node_desc->mac_capability_flags);
    ESP_LOGI(TAG, "[T+%07.3f] NodeDesc short=0x%04X mfg=0x%04X mac_cap=0x%02X", timebase_now_s(), addr, node_desc->manufacturer_code,
             node_desc->mac_capability_flags);
}

static void interview_request_simple_desc(uint16_t short_addr, uint8_t endpoint)
{
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = short_addr,
        .endpoint = endpoint,
    };
    esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, (void *)(uintptr_t)short_addr);
}

static void interview_start(uint16_t short_addr)
{
    esp_zb_zdo_active_ep_req_param_t active_req = {
        .addr_of_interest = short_addr,
    };
    esp_zb_zdo_active_ep_req(&active_req, active_ep_cb, (void *)(uintptr_t)short_addr);
    interview_request_node_desc(short_addr);
    ESP_LOGI(TAG, "[T+%07.3f] Entrevista iniciada para 0x%04X", timebase_now_s(), short_addr);
    emit_event("INTERVIEW_START");
}

static void identity_jobs_poll(void)
{
    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < IDENTITY_JOB_MAX; ++i) {
        identity_job_t *j = &s_identity_jobs[i];
        if (!j->used) {
            continue;
        }
        if (j->got_manufacturer && j->got_model) {
            j->used = false;
            continue;
        }
        if (j->attempts >= IDENTITY_RETRY_MAX) {
            ESP_LOGW(TAG, "[T+%07.3f] Sin identidad completa short=0x%04X ep=%u tras %u intentos", timebase_now_s(), j->short_addr, j->endpoint,
                     j->attempts);
            j->used = false;
            continue;
        }
        if ((int32_t)(now - j->next_try_tick) < 0) {
            continue;
        }

        zcl_read_attr_req(j->short_addr, j->endpoint, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID);
        zcl_read_attr_req(j->short_addr, j->endpoint, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID);
        j->attempts++;
        j->next_try_tick = now + pdMS_TO_TICKS(1800);
        ESP_LOGI(TAG, "[T+%07.3f] Reintento identidad short=0x%04X ep=%u intento=%u/%u", timebase_now_s(), j->short_addr, j->endpoint, j->attempts,
                 IDENTITY_RETRY_MAX);
    }
}

static void emit_event(const char *name)
{
    ESP_LOGI(TAG, "[T+%07.3f] EVENT %s", timebase_now_s(), name);
    if (s_event_cb != NULL) {
        s_event_cb(name);
    }
}

static uint64_t ieee_to_u64(const uint8_t ieee[8])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= ((uint64_t)ieee[i] << (8 * i));
    }
    return v;
}

static uint32_t channel_to_mask(uint8_t channel)
{
    if (channel < 11 || channel > 26) {
        return ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK;
    }
    return (1U << channel);
}

static void refresh_runtime_from_stack(void)
{
    s_runtime.version = ZB_PERSIST_VERSION;
    s_runtime.has_network = esp_zb_bdb_dev_joined();
    s_runtime.channel = esp_zb_get_current_channel();
    s_runtime.pan_id = esp_zb_get_pan_id();
    s_runtime.short_addr = esp_zb_get_short_address();
    esp_zb_get_extended_pan_id(s_runtime.ext_pan_id);
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (callback_id == ESP_ZB_CORE_REPORT_ATTR_CB_ID) {
        const esp_zb_zcl_report_attr_message_t *m = (const esp_zb_zcl_report_attr_message_t *)message;
        zb_trace_meta_t meta = {
            .src_short = (m->src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) ? m->src_address.u.short_addr : 0xFFFF,
            .dst_short = esp_zb_get_short_address(),
            .profile_id = 0x0104,
            .cluster_id = m->cluster,
            .src_ep = m->src_endpoint,
            .dst_ep = m->dst_endpoint,
            .aps_counter = 0,
            .nwk_seq = 0,
            .rssi = 0,
            .lqi = 0,
        };

        uint8_t payload[16] = {0};
        size_t n = 0;
        payload[n++] = (uint8_t)(m->attribute.id & 0xFF);
        payload[n++] = (uint8_t)((m->attribute.id >> 8) & 0xFF);
        payload[n++] = (uint8_t)m->attribute.data.type;
        const size_t copy_len = (m->attribute.data.size < 8U) ? m->attribute.data.size : 8U;
        if (m->attribute.data.value != NULL && copy_len > 0) {
            memcpy(&payload[n], m->attribute.data.value, copy_len);
            n += copy_len;
        }
        (void)zb_trace_log_packet(ZB_DIR_RX, &meta, payload, n);
        device_table_update_from_trace(&meta);
        emit_event("DEVICE_REPORT");
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID) {
        const esp_zb_zcl_cmd_default_resp_message_t *m = (const esp_zb_zcl_cmd_default_resp_message_t *)message;
        zb_trace_meta_t meta = {
            .src_short = (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) ? m->info.src_address.u.short_addr : 0xFFFF,
            .dst_short = m->info.dst_address,
            .profile_id = m->info.profile,
            .cluster_id = m->info.cluster,
            .src_ep = m->info.src_endpoint,
            .dst_ep = m->info.dst_endpoint,
            .aps_counter = m->info.header.tsn,
            .nwk_seq = 0,
            .rssi = m->info.header.rssi,
            .lqi = 0,
        };
        const uint8_t payload[] = {m->info.header.fc, m->info.header.tsn, m->resp_to_cmd, (uint8_t)m->status_code};
        (void)zb_trace_log_packet(ZB_DIR_RX, &meta, payload, sizeof(payload));
        device_table_update_from_trace(&meta);
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *m = (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
        const uint16_t short_addr =
            (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) ? m->info.src_address.u.short_addr : 0xFFFF;
        identity_job_t *job = identity_job_find(short_addr, m->info.src_endpoint);
        char manufacturer[DEVICE_TABLE_MAX_STR] = {0};
        char model[DEVICE_TABLE_MAX_STR] = {0};
        for (const esp_zb_zcl_read_attr_resp_variable_t *v = m->variables; v != NULL; v = v->next) {
            if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS || v->attribute.data.value == NULL) {
                continue;
            }
            if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
                v->attribute.id == ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID &&
                v->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
                attr_to_text(v->attribute.data.value, v->attribute.data.size, manufacturer, sizeof(manufacturer));
                if (job != NULL && manufacturer[0] != '\0') {
                    job->got_manufacturer = true;
                }
            }
            if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
                v->attribute.id == ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID &&
                v->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
                attr_to_text(v->attribute.data.value, v->attribute.data.size, model, sizeof(model));
                if (job != NULL && model[0] != '\0') {
                    job->got_model = true;
                }
            }
        }
        if (manufacturer[0] != '\0' || model[0] != '\0') {
            device_table_update_identity(short_addr, manufacturer, model);
            ESP_LOGI(TAG, "[T+%07.3f] Identidad short=0x%04X manufacturer='%s' model='%s'", timebase_now_s(), short_addr, manufacturer, model);
        }
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID) {
        const esp_zb_zcl_cmd_config_report_resp_message_t *m = (const esp_zb_zcl_cmd_config_report_resp_message_t *)message;
        const uint16_t short_addr =
            (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) ? m->info.src_address.u.short_addr : 0xFFFF;
        ESP_LOGI(TAG, "[T+%07.3f] ConfigureReporting RSP short=0x%04X cluster=0x%04X status=0x%02X", timebase_now_s(), short_addr, m->info.cluster,
                 m->info.status);
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t zb_coordinator_init(const zb_persist_state_t *persist_state, zb_coordinator_event_cb_t event_cb)
{
    s_event_cb = event_cb;
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.version = ZB_PERSIST_VERSION;

    esp_zb_platform_config_t platform_cfg = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg.max_children = 64,
    };
    esp_zb_init(&zb_nwk_cfg);
    ESP_ERROR_CHECK(register_coordinator_endpoint());

    if (persist_state != NULL && persist_state->has_network) {
        esp_zb_set_pan_id(persist_state->pan_id);
        esp_zb_set_extended_pan_id(persist_state->ext_pan_id);
        ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(channel_to_mask(persist_state->channel)));
    } else {
        ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK));
    }

    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_ERROR_CHECK(esp_zb_start(false));
    s_stack_started = true;
    ESP_LOGI(TAG, "[T+%07.3f] Coordinador Zigbee inicializado (SDK real)", timebase_now_s());
    emit_event("STACK_STARTED");
    return ESP_OK;
}

esp_err_t zb_coordinator_set_permit_join(bool enable)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "[T+%07.3f] PermitJoin=%s", timebase_now_s(), enable ? "OPEN" : "CLOSED");
    esp_err_t err = enable ? esp_zb_bdb_open_network(180) : esp_zb_bdb_close_network();
    esp_zb_lock_release();
    emit_event(enable ? "PERMIT_JOIN_OPEN" : "PERMIT_JOIN_CLOSED");
    return err;
}

esp_err_t zb_coordinator_poll(void)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_zb_stack_main_loop_iteration();
    identity_jobs_poll();
    refresh_runtime_from_stack();
    esp_zb_lock_release();
    return ESP_OK;
}

esp_err_t zb_coordinator_get_runtime_state(zb_persist_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_state = s_runtime;
    return ESP_OK;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_s)
{
    if (signal_s == NULL || signal_s->p_app_signal == NULL) {
        return;
    }

    const esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)(*signal_s->p_app_signal);
    const esp_err_t st = signal_s->esp_err_status;
    const char *sig_name = esp_zb_zdo_signal_to_string(sig);
    ESP_LOGI(TAG, "[T+%07.3f] ZB_SIGNAL %s status=%d", timebase_now_s(), sig_name ? sig_name : "UNKNOWN", (int)st);

    if (sig == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP) {
        (void)esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        emit_event("BDB_FORMATION_START");
        return;
    }

    if (sig == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START || sig == ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT) {
        if (st == ESP_OK && esp_zb_bdb_is_factory_new()) {
            (void)esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            emit_event("FACTORY_NEW_FORMATION");
        } else if (st == ESP_OK) {
            emit_event("NETWORK_RESTORED");
        }
        return;
    }

    if (sig == ESP_ZB_BDB_SIGNAL_FORMATION && st == ESP_OK) {
        emit_event("NETWORK_FORMED");
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE) {
        const esp_zb_zdo_signal_device_annce_params_t *p =
            (const esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            const uint64_t ieee = ieee_to_u64(p->ieee_addr);
            device_table_update_discovery(ieee, p->device_short_addr, 0, NULL, NULL);

            zb_trace_meta_t m = {
                .src_short = p->device_short_addr,
                .dst_short = 0x0000,
                .profile_id = 0x0000,
                .cluster_id = 0x0013,
                .src_ep = 0,
                .dst_ep = 0,
                .aps_counter = 0,
                .nwk_seq = 0,
                .rssi = 0,
                .lqi = 0,
            };
            uint8_t payload[11] = {0};
            payload[0] = (uint8_t)(p->device_short_addr & 0xFF);
            payload[1] = (uint8_t)(p->device_short_addr >> 8);
            memcpy(&payload[2], p->ieee_addr, 8);
            payload[10] = p->capability;
            (void)zb_trace_log_packet(ZB_DIR_RX, &m, payload, sizeof(payload));
            interview_start(p->device_short_addr);
        }
        emit_event("DEVICE_ANNOUNCE");
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE) {
        const esp_zb_zdo_signal_device_update_params_t *p =
            (const esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            const uint64_t ieee = ieee_to_u64(p->long_addr);
            device_table_update_discovery(ieee, p->short_addr, 0, NULL, NULL);
            emit_event("DEVICE_UPDATE");
        }
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION) {
        emit_event("DEVICE_LEAVE");
    }
}
