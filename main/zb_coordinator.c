#include "zb_coordinator.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
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
#include "zcl/esp_zigbee_zcl_illuminance_meas.h"
#include "zcl/esp_zigbee_zcl_pressure_meas.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_ias_zone.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "timebase.h"
#include "zb_trace.h"

static const char *TAG = "zb_coord";
static zb_network_runtime_t s_runtime;
static zb_coordinator_event_cb_t s_event_cb = NULL;
static bool s_stack_started = false;
static const uint8_t COORDINATOR_ENDPOINT = 1;
static const uint32_t ZB_COORD_PRIMARY_CHANNEL_MASK = 0x01FFF800U; /* canales 11-24 */
static const char *ZB_COORD_PRIMARY_CHANNEL_POLICY = "11-24";
static void emit_event(const char *name);
static const uint32_t ZB_COORD_RUNTIME_VERSION = 1U;

#define IDENTITY_JOB_MAX 16
#define IDENTITY_RETRY_MAX 4
#define INTERVIEW_JOB_MAX 32
#define REQ_TRACK_MAX 64
#define IEEE_RESOLVE_MAX 32
#define PENDING_INTERVIEW_MAX 16
#define SILENT_TIMEOUT_S 120.0
#define ABSENT_TIMEOUT_S 420.0
#define INTERVIEW_RESTART_GUARD_S 10.0
#define HEALTH_POLL_PERIOD_MS 2000
/** Sondeo Read Attribute sobre sensores conocidos como red de seguridad. */
#define SENSOR_POLL_PERIOD_MS 120000

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

typedef struct {
    bool used;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t phase;
    uint8_t retries;
    uint8_t ep_count;
    uint8_t ep_idx;
    uint8_t ep_list[DEVICE_TABLE_MAX_ENDPOINTS];
    TickType_t next_tick;
    double last_seen_s;
    double last_interview_s;
    bool node_desc_ok;
    bool active_ep_ok;
    bool simple_desc_ok;
} interview_job_t;

typedef struct {
    bool used;
    bool is_zdo;
    uint16_t short_addr;
    uint8_t tsn;
    double sent_s;
} req_track_t;

static interview_job_t s_interview_jobs[INTERVIEW_JOB_MAX];
static req_track_t s_req_track[REQ_TRACK_MAX];
static TickType_t s_next_health_tick = 0;

typedef struct {
    bool used;
    uint16_t short_addr;
    TickType_t next_try_tick;
    uint8_t tries;
} ieee_resolve_job_t;

static ieee_resolve_job_t s_ieee_resolve[IEEE_RESOLVE_MAX];
static TickType_t s_next_sensor_poll_tick = 0;
static uint16_t s_pending_interview[PENDING_INTERVIEW_MAX];
static size_t s_pending_interview_n;

static void interview_start(uint16_t short_addr);
static bool aps_data_indication_handler(esp_zb_apsde_data_ind_t ind);

static TickType_t delay_with_jitter_ms(uint32_t base_ms, uint32_t span_ms, uint16_t short_addr, uint8_t salt)
{
    uint32_t jitter = 0;
    if (span_ms != 0U) {
        jitter = (((uint32_t)short_addr * 37U) + ((uint32_t)salt * 53U)) % span_ms;
    }
    return xTaskGetTickCount() + pdMS_TO_TICKS(base_ms + jitter);
}

static bool pending_interview_enqueue(uint16_t short_addr)
{
    if (short_addr == 0x0000U || short_addr == 0xFFFFU) {
        return false;
    }
    for (size_t i = 0; i < s_pending_interview_n; ++i) {
        if (s_pending_interview[i] == short_addr) {
            return false;
        }
    }
    if (s_pending_interview_n < PENDING_INTERVIEW_MAX) {
        s_pending_interview[s_pending_interview_n++] = short_addr;
        return true;
    }
    return false;
}

static void pending_interview_drain(void)
{
    if (s_pending_interview_n == 0) {
        return;
    }
    uint16_t batch[PENDING_INTERVIEW_MAX];
    const size_t n = s_pending_interview_n;
    memcpy(batch, s_pending_interview, n * sizeof(uint16_t));
    s_pending_interview_n = 0;
    for (size_t i = 0; i < n; ++i) {
        interview_start(batch[i]);
    }
}

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

static interview_job_t *interview_find(uint16_t short_addr)
{
    for (size_t i = 0; i < INTERVIEW_JOB_MAX; ++i) {
        if (s_interview_jobs[i].used && s_interview_jobs[i].short_addr == short_addr) {
            return &s_interview_jobs[i];
        }
    }
    return NULL;
}

static void pending_interview_remove(uint16_t short_addr)
{
    for (size_t i = 0; i < s_pending_interview_n; ++i) {
        if (s_pending_interview[i] != short_addr) {
            continue;
        }
        for (size_t j = i + 1; j < s_pending_interview_n; ++j) {
            s_pending_interview[j - 1] = s_pending_interview[j];
        }
        s_pending_interview_n--;
        return;
    }
}

static void interview_cancel(uint16_t short_addr, const char *reason)
{
    pending_interview_remove(short_addr);
    for (size_t i = 0; i < INTERVIEW_JOB_MAX; ++i) {
        interview_job_t *j = &s_interview_jobs[i];
        if (!j->used || j->short_addr != short_addr) {
            continue;
        }
        memset(j, 0, sizeof(*j));
    }
    for (size_t i = 0; i < IDENTITY_JOB_MAX; ++i) {
        identity_job_t *j = &s_identity_jobs[i];
        if (!j->used || j->short_addr != short_addr) {
            continue;
        }
        memset(j, 0, sizeof(*j));
    }
    ESP_LOGW(TAG, "[T+%07.3f] Entrevista cancelada short=0x%04X motivo=%s", timebase_now_s(), short_addr, reason != NULL ? reason : "n/a");
}

static bool interview_job_complete(const interview_job_t *job)
{
    return (job != NULL && job->node_desc_ok && job->active_ep_ok && job->simple_desc_ok && job->phase >= 4U);
}

static bool interview_job_has_useful_profile(const interview_job_t *job)
{
    return (job != NULL && (job->simple_desc_ok || job->node_desc_ok || job->active_ep_ok));
}

static bool interview_request_enqueue(uint16_t short_addr, const char *reason)
{
    if (short_addr == 0x0000U || short_addr == 0xFFFFU) {
        return false;
    }
    interview_job_t *job = interview_find(short_addr);
    const double now_s = timebase_now_s();
    if (job != NULL) {
        const bool recent_seen = (job->last_seen_s > 0.0 && (now_s - job->last_seen_s) < INTERVIEW_RESTART_GUARD_S);
        const bool recent_done = (job->last_interview_s > 0.0 && (now_s - job->last_interview_s) < INTERVIEW_RESTART_GUARD_S);
        const bool in_progress = (job->phase != 0U && !interview_job_complete(job) && job->phase != 0xFFU);
        if (recent_seen || recent_done || in_progress) {
            ESP_LOGI(TAG,
                     "[T+%07.3f] Entrevista suprimida short=0x%04X motivo=%s phase=%u retries=%u complete=%s",
                     now_s, short_addr, reason != NULL ? reason : "n/a", (unsigned)job->phase, (unsigned)job->retries,
                     interview_job_complete(job) ? "true" : "false");
            return false;
        }
    }
    if (pending_interview_enqueue(short_addr)) {
        ESP_LOGI(TAG, "[T+%07.3f] Entrevista encolada short=0x%04X motivo=%s", now_s, short_addr, reason != NULL ? reason : "n/a");
        return true;
    }
    return false;
}

void zb_coordinator_note_inbound_device_traffic(uint16_t short_addr)
{
    if (short_addr == 0x0000U || short_addr == 0xFFFFU) {
        return;
    }
    interview_job_t *job = interview_find(short_addr);
    if (job != NULL) {
        job->last_seen_s = timebase_now_s();
    }
}

static interview_job_t *interview_get_or_create(uint16_t short_addr)
{
    interview_job_t *j = interview_find(short_addr);
    if (j != NULL) {
        return j;
    }
    for (size_t i = 0; i < INTERVIEW_JOB_MAX; ++i) {
        if (!s_interview_jobs[i].used) {
            memset(&s_interview_jobs[i], 0, sizeof(s_interview_jobs[i]));
            s_interview_jobs[i].used = true;
            s_interview_jobs[i].short_addr = short_addr;
            return &s_interview_jobs[i];
        }
    }
    return NULL;
}

static void req_track_sent(bool is_zdo, uint16_t short_addr, uint8_t tsn)
{
    for (size_t i = 0; i < REQ_TRACK_MAX; ++i) {
        if (!s_req_track[i].used) {
            s_req_track[i].used = true;
            s_req_track[i].is_zdo = is_zdo;
            s_req_track[i].short_addr = short_addr;
            s_req_track[i].tsn = tsn;
            s_req_track[i].sent_s = timebase_now_s();
            return;
        }
    }
}

static void req_track_ack(bool is_zdo, uint16_t short_addr, uint8_t tsn)
{
    for (size_t i = 0; i < REQ_TRACK_MAX; ++i) {
        if (!s_req_track[i].used || s_req_track[i].is_zdo != is_zdo) {
            continue;
        }
        if (s_req_track[i].short_addr == short_addr && s_req_track[i].tsn == tsn) {
            const double latency_ms = (timebase_now_s() - s_req_track[i].sent_s) * 1000.0;
            device_table_note_latency(is_zdo, latency_ms);
            s_req_track[i].used = false;
            return;
        }
    }
}

static ieee_resolve_job_t *ieee_resolve_find(uint16_t short_addr)
{
    for (size_t i = 0; i < IEEE_RESOLVE_MAX; ++i) {
        if (s_ieee_resolve[i].used && s_ieee_resolve[i].short_addr == short_addr) {
            return &s_ieee_resolve[i];
        }
    }
    return NULL;
}

static void ieee_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx)
{
    const uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    ieee_resolve_job_t *j = ieee_resolve_find(short_addr);
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS && resp != NULL) {
        uint64_t ieee = 0;
        for (int i = 0; i < 8; ++i) {
            ieee |= ((uint64_t)resp->ieee_addr[i] << (8 * i));
        }
        device_table_update_discovery(ieee, resp->nwk_addr, 0, NULL, NULL);
        zb_coordinator_note_inbound_device_traffic(resp->nwk_addr);
        /* Misma secuencia que tras DEVICE_ANNCE: entrevista completa fuera del callback ZDO. */
        (void)interview_request_enqueue(resp->nwk_addr, "ieee_resolved");
        if (j != NULL) {
            j->used = false;
        }
        ESP_LOGI(TAG, "[T+%07.3f] IEEE resuelto short=0x%04X ieee=0x%016llX (entrevista encolada)", timebase_now_s(), short_addr,
                 (unsigned long long)ieee);
        return;
    }
    if (j != NULL) {
        j->tries++;
        j->next_try_tick = delay_with_jitter_ms(1200U + ((uint32_t)j->tries * 700U), 400U, short_addr, j->tries);
    }
    ESP_LOGW(TAG, "[T+%07.3f] IEEE resolve fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
}

static void ieee_resolve_schedule(uint16_t short_addr)
{
    if (short_addr == 0x0000 || short_addr == 0xFFFF) {
        return;
    }
    bool created = false;
    ieee_resolve_job_t *j = ieee_resolve_find(short_addr);
    if (j == NULL) {
        for (size_t i = 0; i < IEEE_RESOLVE_MAX; ++i) {
            if (!s_ieee_resolve[i].used) {
                j = &s_ieee_resolve[i];
                memset(j, 0, sizeof(*j));
                j->used = true;
                j->short_addr = short_addr;
                created = true;
                break;
            }
        }
    }
    if (j == NULL) {
        return;
    }
    if (!created && (j->tries > 0U || j->next_try_tick != 0)) {
        return;
    }
    j->next_try_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "[T+%07.3f] IEEE resolve programado short=0x%04X", timebase_now_s(), short_addr);
}

static void ieee_resolve_poll(void)
{
    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < IEEE_RESOLVE_MAX; ++i) {
        ieee_resolve_job_t *j = &s_ieee_resolve[i];
        if (!j->used) {
            continue;
        }
        if (j->tries >= 5) {
            j->used = false;
            continue;
        }
        if ((int32_t)(now - j->next_try_tick) < 0) {
            continue;
        }
        esp_zb_zdo_ieee_addr_req_param_t req = {
            .dst_nwk_addr = j->short_addr,
            .addr_of_interest = j->short_addr,
            .request_type = 0,
            .start_index = 0,
        };
        esp_zb_zdo_ieee_addr_req(&req, ieee_addr_cb, (void *)(uintptr_t)j->short_addr);
        j->tries++;
        j->next_try_tick = delay_with_jitter_ms(1000U + ((uint32_t)j->tries * 700U), 400U, j->short_addr, j->tries);
    }
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

/** Interpreta atributo ZCL y actualiza lecturas en la tabla (informes y Read RSP). @return true si la lectura almacenada cambio. */
static bool zcl_apply_attribute_to_readings(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, const esp_zb_zcl_attribute_t *attr)
{
    if (attr == NULL || attr->data.value == NULL || attr->data.size == 0U) {
        return false;
    }
    const void *pv = attr->data.value;
    const esp_zb_zcl_attr_type_t ty = attr->data.type;

    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr->id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID) {
        if (attr->data.size >= 2U && (ty == ESP_ZB_ZCL_ATTR_TYPE_S16 || ty == ESP_ZB_ZCL_ATTR_TYPE_U16)) {
            int16_t raw;
            memcpy(&raw, pv, sizeof(raw));
            if (device_table_note_reading_temperature(short_addr, ep, raw)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor temp short=0x%04X ep=%u -> %.2f C", timebase_now_s(), short_addr, ep, raw / 100.0);
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr->id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID) {
        if (attr->data.size >= 2U && (ty == ESP_ZB_ZCL_ATTR_TYPE_U16 || ty == ESP_ZB_ZCL_ATTR_TYPE_S16)) {
            uint16_t raw;
            memcpy(&raw, pv, sizeof(raw));
            if (device_table_note_reading_humidity(short_addr, ep, raw)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor humedad short=0x%04X ep=%u -> %.2f %%", timebase_now_s(), short_addr, ep, raw / 100.0);
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && attr->id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        if (ty == ESP_ZB_ZCL_ATTR_TYPE_BOOL && attr->data.size >= 1U) {
            const uint8_t b = *(const uint8_t *)pv;
            if (device_table_note_reading_on_off(short_addr, ep, b != 0U)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor on_off short=0x%04X ep=%u -> %s", timebase_now_s(), short_addr, ep, b ? "ON" : "OFF");
                return true;
            }
            return false;
        }
        if (ty == ESP_ZB_ZCL_ATTR_TYPE_U8 && attr->data.size >= 1U) {
            const uint8_t b = *(const uint8_t *)pv;
            if (device_table_note_reading_on_off(short_addr, ep, b != 0U)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor on_off short=0x%04X ep=%u -> %s", timebase_now_s(), short_addr, ep, b ? "ON" : "OFF");
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING && attr->id == ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID) {
        if ((ty == ESP_ZB_ZCL_ATTR_TYPE_U8 || ty == ESP_ZB_ZCL_ATTR_TYPE_8BITMAP) && attr->data.size >= 1U) {
            const uint8_t occ = *(const uint8_t *)pv;
            if (device_table_note_reading_occupancy(short_addr, ep, occ)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor ocupacion short=0x%04X ep=%u bitmap=0x%02X ocupado=%s", timebase_now_s(), short_addr, ep, occ,
                         (occ & 1U) ? "si" : "no");
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT &&
        attr->id == ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID) {
        if (attr->data.size >= 2U && (ty == ESP_ZB_ZCL_ATTR_TYPE_U16 || ty == ESP_ZB_ZCL_ATTR_TYPE_S16)) {
            uint16_t raw;
            memcpy(&raw, pv, sizeof(raw));
            if (device_table_note_reading_illuminance(short_addr, ep, raw)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor iluminancia short=0x%04X ep=%u measured_value=%u", timebase_now_s(), short_addr, ep,
                         (unsigned)raw);
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && attr->id == ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID) {
        if (attr->data.size >= 2U && (ty == ESP_ZB_ZCL_ATTR_TYPE_S16 || ty == ESP_ZB_ZCL_ATTR_TYPE_U16)) {
            int16_t raw;
            memcpy(&raw, pv, sizeof(raw));
            if (device_table_note_reading_pressure(short_addr, ep, raw)) {
                ESP_LOGI(TAG, "[T+%07.3f] Valor presion short=0x%04X ep=%u raw_0_1_kpa=%d (%.2f kPa)", timebase_now_s(), short_addr, ep, (int)raw,
                         (double)raw / 10.0);
                return true;
            }
        }
        return false;
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG) {
        if (attr->id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID) {
            if (ty == ESP_ZB_ZCL_ATTR_TYPE_U8 && attr->data.size >= 1U) {
                const uint8_t v = *(const uint8_t *)pv;
                if (device_table_note_reading_battery_voltage(short_addr, ep, v)) {
                    ESP_LOGI(TAG, "[T+%07.3f] Bateria (voltaje) short=0x%04X ep=%u raw_100mV=%u -> %u mV", timebase_now_s(), short_addr, ep,
                             (unsigned)v, (unsigned)v * 100U);
                    return true;
                }
            }
            return false;
        }
        if (attr->id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID) {
            if (ty == ESP_ZB_ZCL_ATTR_TYPE_U8 && attr->data.size >= 1U) {
                const uint8_t p = *(const uint8_t *)pv;
                if (device_table_note_reading_battery_pct(short_addr, ep, p)) {
                    ESP_LOGI(TAG, "[T+%07.3f] Bateria (%%) short=0x%04X ep=%u raw_medios_puntos=%u -> ~%u %%", timebase_now_s(), short_addr, ep,
                             (unsigned)p, (unsigned)(p / 2U));
                    return true;
                }
            }
            return false;
        }
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE && attr->id == ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID) {
        if (attr->data.size >= 2U && (ty == ESP_ZB_ZCL_ATTR_TYPE_U16 || ty == ESP_ZB_ZCL_ATTR_TYPE_16BITMAP)) {
            uint16_t zs;
            memcpy(&zs, pv, sizeof(zs));
            if (device_table_note_reading_ias_zone_status(short_addr, ep, zs)) {
                ESP_LOGI(TAG, "[T+%07.3f] IAS ZoneStatus short=0x%04X ep=%u status=0x%04X", timebase_now_s(), short_addr, ep, (unsigned)zs);
                return true;
            }
        }
    }
    return false;
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
    device_table_inc_counter("read_req");
    req_track_sent(false, dst_short, tsn);
    ESP_LOGI(TAG, "[T+%07.3f] ReadAttr REQ short=0x%04X ep=%u cl=0x%04X attr=0x%04X tsn=0x%02X", timebase_now_s(), dst_short, dst_ep,
             cluster_id, attr_id, tsn);
}

static void sensor_poll_issue_read(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t attr_id)
{
    device_table_note_poll_request(short_addr, ep);
    zcl_read_attr_req(short_addr, ep, cluster_id, attr_id);
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
    device_table_inc_counter("report_cfg_req");
    req_track_sent(false, dst_short, tsn);
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

    esp_zb_on_off_cluster_cfg_t on_off_cfg = {
        .on_off = false,
    };
    esp_zb_attribute_list_t *on_off_attr = esp_zb_on_off_cluster_create(&on_off_cfg);
    if (on_off_attr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cluster_list, on_off_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

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
    const uint32_t packed = (uint32_t)(uintptr_t)user_ctx;
    const uint16_t short_addr = (uint16_t)((packed >> 8) & 0xFFFF);
    const uint8_t req_ep = (uint8_t)(packed & 0xFF);
    zb_coordinator_note_inbound_device_traffic(short_addr);
    interview_job_t *job = interview_find(short_addr);
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL) {
        if (interview_job_complete(job)) {
            ESP_LOGI(TAG, "[T+%07.3f] Entrevista simple desc tardia ignorada short=0x%04X status=0x%02X", timebase_now_s(), short_addr,
                     zdo_status);
            return;
        }
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista simple desc fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
        if (job != NULL) {
            job->phase = 3;
            job->simple_desc_ok = false;
            job->retries++;
            job->next_tick = delay_with_jitter_ms(1400U + ((uint32_t)job->retries * 700U), 500U, short_addr, job->retries);
        }
        device_table_mark_interview(short_addr, 3, false, true);
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

    device_table_update_simple_desc(short_addr, simple_desc->endpoint, simple_desc->app_profile_id, simple_desc->app_device_id,
                                    simple_desc->app_device_version, clusters_in, in_len, clusters_out, out_len);
    ESP_LOGI(TAG, "[T+%07.3f] Entrevista EP=%u short=0x%04X device_id=0x%04X in=%u out=%u", timebase_now_s(), simple_desc->endpoint, short_addr,
             simple_desc->app_device_id, in_count, out_count);
    emit_event("INTERVIEW_SIMPLE_DESC");
    if (job != NULL) {
        job->phase = 3;
        job->simple_desc_ok = true;
        if (job->ep_idx < job->ep_count && job->ep_list[job->ep_idx] == req_ep) {
            job->ep_idx++;
        }
        if (job->ep_idx >= job->ep_count) {
            job->last_interview_s = timebase_now_s();
            job->retries = 0;
            job->phase = 4;
            job->next_tick = 0;
            device_table_mark_interview(short_addr, 4, true, false);
        }
    }

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
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT)) {
        static const uint16_t ill_delta = 1000U;
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                          ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                              ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_U16, 30, 300, &ill_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Sensor iluminancia short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT)) {
        static const int16_t pres_delta = 10;
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,
                          ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,
                              ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, 30, 300, &pres_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Sensor presion short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG)) {
        static const uint8_t v_delta = 1U;
        static const uint8_t pct_delta = 2U;
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                          ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                          ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                              ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, ESP_ZB_ZCL_ATTR_TYPE_U8, 60, 3600, &v_delta);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                              ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, ESP_ZB_ZCL_ATTR_TYPE_U8, 60, 3600, &pct_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] Power Config (bateria) short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
    if (has_cluster(clusters_in, in_len, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE)) {
        static const uint16_t zs_delta = 1U;
        zcl_read_attr_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE, ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID);
        zcl_config_report_req(short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE, ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID,
                              ESP_ZB_ZCL_ATTR_TYPE_16BITMAP, 10, 300, &zs_delta);
        emit_event("INTERVIEW_CONFIG");
        ESP_LOGI(TAG, "[T+%07.3f] IAS Zone short=0x%04X ep=%u", timebase_now_s(), short_addr, simple_desc->endpoint);
    }
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    const uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    zb_coordinator_note_inbound_device_traffic(short_addr);
    interview_job_t *job = interview_find(short_addr);
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_id_list == NULL || ep_count == 0) {
        if (interview_job_complete(job) || interview_job_has_useful_profile(job)) {
            ESP_LOGI(TAG, "[T+%07.3f] Entrevista active_ep tardia ignorada short=0x%04X status=0x%02X", timebase_now_s(), short_addr,
                     zdo_status);
            return;
        }
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista active_ep fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
        if (job != NULL) {
            job->phase = 2;
            job->active_ep_ok = false;
            job->retries++;
            job->next_tick = delay_with_jitter_ms(1000U + ((uint32_t)job->retries * 600U), 400U, short_addr, job->retries);
        }
        device_table_mark_interview(short_addr, 2, false, true);
        return;
    }

    ESP_LOGI(TAG, "[T+%07.3f] Entrevista short=0x%04X endpoints=%u", timebase_now_s(), short_addr, ep_count);
    if (job != NULL) {
        job->phase = 2;
        job->active_ep_ok = true;
        job->ep_count = (ep_count > DEVICE_TABLE_MAX_ENDPOINTS) ? DEVICE_TABLE_MAX_ENDPOINTS : ep_count;
        job->ep_idx = 0;
        memcpy(job->ep_list, ep_id_list, job->ep_count);
    }
    for (uint8_t i = 0; i < ep_count; ++i) {
        interview_request_simple_desc(short_addr, ep_id_list[i]);
    }
}

static void node_desc_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, esp_zb_af_node_desc_t *node_desc, void *user_ctx)
{
    const uint16_t short_addr = (addr != 0x0000U && addr != 0xFFFFU) ? addr : (uint16_t)(uintptr_t)user_ctx;
    zb_coordinator_note_inbound_device_traffic(short_addr);
    interview_job_t *job = interview_find(short_addr);
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || node_desc == NULL) {
        if (interview_job_complete(job) || interview_job_has_useful_profile(job)) {
            ESP_LOGI(TAG, "[T+%07.3f] Entrevista node_desc tardia ignorada short=0x%04X status=0x%02X", timebase_now_s(), short_addr,
                     zdo_status);
            return;
        }
        ESP_LOGW(TAG, "[T+%07.3f] Entrevista node_desc fallo short=0x%04X status=0x%02X", timebase_now_s(), short_addr, zdo_status);
        if (job != NULL) {
            job->phase = 1;
            job->node_desc_ok = false;
            job->retries++;
            job->next_tick = delay_with_jitter_ms(1000U + ((uint32_t)job->retries * 600U), 400U, short_addr, job->retries);
        }
        device_table_mark_interview(short_addr, 1, false, true);
        return;
    }
    device_table_update_node_desc(short_addr, node_desc->node_desc_flags, node_desc->mac_capability_flags, node_desc->manufacturer_code,
                                  node_desc->max_buf_size, node_desc->max_incoming_transfer_size, node_desc->server_mask,
                                  node_desc->max_outgoing_transfer_size, node_desc->desc_capability_field);
    if (job != NULL) {
        job->phase = 1;
        job->node_desc_ok = true;
        job->last_seen_s = timebase_now_s();
    }
    ESP_LOGI(TAG, "[T+%07.3f] NodeDesc short=0x%04X mfg=0x%04X mac_cap=0x%02X", timebase_now_s(), short_addr, node_desc->manufacturer_code,
             node_desc->mac_capability_flags);
}

static void interview_request_simple_desc(uint16_t short_addr, uint8_t endpoint)
{
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = short_addr,
        .endpoint = endpoint,
    };
    const uint32_t packed = (((uint32_t)short_addr) << 8) | endpoint;
    esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, (void *)(uintptr_t)packed);
}

static void interview_start(uint16_t short_addr)
{
    interview_job_t *job = interview_get_or_create(short_addr);
    const double now_s = timebase_now_s();
    if (job != NULL) {
        const bool recent_seen = (job->last_seen_s > 0.0 && (now_s - job->last_seen_s) < INTERVIEW_RESTART_GUARD_S);
        const bool recent_done = (job->last_interview_s > 0.0 && (now_s - job->last_interview_s) < INTERVIEW_RESTART_GUARD_S);
        const bool in_progress = (job->phase != 0U && !interview_job_complete(job) && job->phase != 0xFFU);
        if (recent_seen || recent_done || in_progress) {
            ESP_LOGI(TAG,
                     "[T+%07.3f] Entrevista omitida short=0x%04X phase=%u retries=%u complete=%s",
                     now_s, short_addr, (unsigned)job->phase, (unsigned)job->retries, interview_job_complete(job) ? "true" : "false");
            return;
        }
    }
    if (job != NULL) {
        job->retries = 0;
        job->phase = 1;
        job->ep_count = 0;
        job->ep_idx = 0;
        job->node_desc_ok = false;
        job->active_ep_ok = false;
        job->simple_desc_ok = false;
        job->last_seen_s = now_s;
        job->next_tick = delay_with_jitter_ms(1000U, 400U, short_addr, 1U);
        device_table_mark_interview(short_addr, 1, false, false);
    }

    esp_zb_zdo_active_ep_req_param_t active_req = {
        .addr_of_interest = short_addr,
    };
    esp_zb_zdo_active_ep_req(&active_req, active_ep_cb, (void *)(uintptr_t)short_addr);
    interview_request_node_desc(short_addr);
    ESP_LOGI(TAG, "[T+%07.3f] Entrevista iniciada para 0x%04X", now_s, short_addr);
    emit_event("INTERVIEW_START");
}

static void recontact_known_devices(void)
{
    uint16_t shorts[DEVICE_TABLE_MAX_DEVICES] = {0};
    const size_t n = device_table_get_known_short_addrs(shorts, DEVICE_TABLE_MAX_DEVICES);
    if (n == 0) {
        ESP_LOGI(TAG, "[T+%07.3f] Reanudacion: no hay dispositivos cacheados", timebase_now_s());
        return;
    }
    ESP_LOGI(TAG, "[T+%07.3f] Red restaurada con %u dispositivos conocidos; se conserva cache sin reentrevista masiva", timebase_now_s(),
             (unsigned)n);
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
        j->next_try_tick = delay_with_jitter_ms(1800U, 500U, j->short_addr, j->attempts);
        ESP_LOGI(TAG, "[T+%07.3f] Reintento identidad short=0x%04X ep=%u intento=%u/%u", timebase_now_s(), j->short_addr, j->endpoint, j->attempts,
                 IDENTITY_RETRY_MAX);
    }
}

static void health_and_interview_poll(void)
{
    const TickType_t now_tick = xTaskGetTickCount();
    const double now_s = timebase_now_s();
    if (s_next_health_tick == 0 || (int32_t)(now_tick - s_next_health_tick) >= 0) {
        s_next_health_tick = now_tick + pdMS_TO_TICKS(HEALTH_POLL_PERIOD_MS);
    } else {
        return;
    }

    for (size_t i = 0; i < INTERVIEW_JOB_MAX; ++i) {
        interview_job_t *j = &s_interview_jobs[i];
        if (!j->used) {
            continue;
        }

        if ((now_s - j->last_seen_s) > SILENT_TIMEOUT_S) {
            device_table_mark_silent(j->short_addr, true);
            if ((now_s - j->last_seen_s) > ABSENT_TIMEOUT_S) {
                device_table_mark_absent_prolonged(j->short_addr, true);
                emit_event("NODE_ABSENT_PROLONGED");
            }
        } else {
            device_table_mark_silent(j->short_addr, false);
            device_table_mark_absent_prolonged(j->short_addr, false);
        }

        if ((int32_t)(now_tick - j->next_tick) < 0) {
            continue;
        }

        if (!(j->node_desc_ok && j->active_ep_ok && j->simple_desc_ok)) {
            if (j->retries >= 5) {
                j->phase = 0xFF;
                device_table_mark_interview(j->short_addr, 0xFF, false, true);
                j->next_tick = 0;
                j->retries = 0;
                continue;
            }
            interview_request_node_desc(j->short_addr);
            esp_zb_zdo_active_ep_req_param_t active_req = {
                .addr_of_interest = j->short_addr,
            };
            esp_zb_zdo_active_ep_req(&active_req, active_ep_cb, (void *)(uintptr_t)j->short_addr);
            j->retries++;
            device_table_mark_interview(j->short_addr, 2, false, true);
            j->next_tick = delay_with_jitter_ms(1200U + ((uint32_t)j->retries * 900U), 500U, j->short_addr, j->retries);
        }
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

static void refresh_runtime_from_stack(void)
{
    s_runtime.version = ZB_COORD_RUNTIME_VERSION;
    s_runtime.has_network = esp_zb_bdb_dev_joined();
    s_runtime.channel = esp_zb_get_current_channel();
    s_runtime.pan_id = esp_zb_get_pan_id();
    s_runtime.short_addr = esp_zb_get_short_address();
    esp_zb_get_extended_pan_id(s_runtime.ext_pan_id);
}

static void log_network_runtime_summary(const char *reason)
{
    refresh_runtime_from_stack();
    ESP_LOGI(TAG, "[T+%07.3f] Red %s: channel=%u pan_id=0x%04X short=0x%04X", timebase_now_s(), reason != NULL ? reason : "n/a",
             (unsigned)s_runtime.channel, s_runtime.pan_id, s_runtime.short_addr);
}

/** RSSI/LQI del ultimo paquete no vienen en esp_zb_zcl_report_attr_message_t; se toman de la tabla de vecinos NWK. */
static bool neighbor_table_lookup_link(uint16_t short_addr, int8_t *rssi_out, uint8_t *lqi_out)
{
    if (short_addr == 0x0000U || short_addr == 0xFFFFU || rssi_out == NULL || lqi_out == NULL) {
        return false;
    }
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    esp_zb_nwk_neighbor_info_t nbr;
    for (;;) {
        const esp_err_t err = esp_zb_nwk_get_next_neighbor(&it, &nbr);
        if (err != ESP_OK) {
            break;
        }
        if (nbr.short_addr == short_addr) {
            *rssi_out = nbr.rssi;
            *lqi_out = nbr.lqi;
            return true;
        }
    }
    return false;
}

static void trace_meta_fill_neighbor_link(zb_trace_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    int8_t r = 0;
    uint8_t l = 0;
    if (neighbor_table_lookup_link(meta->src_short, &r, &l)) {
        meta->rssi = r;
        meta->lqi = l;
    }
}

static bool aps_data_indication_handler(esp_zb_apsde_data_ind_t ind)
{
    if (ind.status != 0U || ind.src_short_addr == 0x0000U || ind.src_short_addr == 0xFFFFU) {
        return false;
    }

    zb_trace_meta_t meta = {
        .src_short = ind.src_short_addr,
        .dst_short = ind.dst_short_addr,
        .profile_id = ind.profile_id,
        .cluster_id = ind.cluster_id,
        .src_ep = ind.src_endpoint,
        .dst_ep = ind.dst_endpoint,
        .aps_counter = 0,
        .nwk_seq = 0,
        .rssi = 0,
        .lqi = (ind.lqi < 0) ? 0U : (uint8_t)ind.lqi,
    };
    trace_meta_fill_neighbor_link(&meta);
    device_table_update_rf_metrics(ind.src_short_addr, meta.rssi, meta.lqi);
    zb_coordinator_note_inbound_device_traffic(ind.src_short_addr);

    const bool known_device = device_table_has_short_addr(ind.src_short_addr);
    if (!known_device) {
        const size_t copy_len = (ind.asdu_length < 16U) ? ind.asdu_length : 16U;
        if (ind.asdu != NULL && copy_len > 0U) {
            (void)zb_trace_log_packet(ZB_DIR_RX, &meta, ind.asdu, copy_len);
        }
        ESP_LOGI(TAG,
                 "[T+%07.3f] APS nodo desconocido short=0x%04X profile=0x%04X cluster=0x%04X src_ep=%u dst_ep=%u (IEEE resolve solicitado)",
                 timebase_now_s(), ind.src_short_addr, ind.profile_id, ind.cluster_id, ind.src_endpoint, ind.dst_endpoint);
        ieee_resolve_schedule(ind.src_short_addr);
    }

    if (ind.profile_id != ESP_ZB_AF_HA_PROFILE_ID || ind.dst_endpoint != COORDINATOR_ENDPOINT || ind.asdu == NULL || ind.asdu_length < 3U) {
        return false;
    }

    const uint8_t fc = ind.asdu[0];
    const bool manufacturer_specific = ((fc & 0x04U) != 0U);
    const uint8_t frame_type = (fc & 0x03U);
    const size_t cmd_idx = manufacturer_specific ? 4U : 2U;
    if (frame_type != 0x01U || ind.asdu_length <= cmd_idx) {
        return false;
    }

    const uint8_t cmd_id = ind.asdu[cmd_idx];
    if (ind.cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        (cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID || cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_ID || cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID ||
         cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID || cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID ||
         cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID)) {
        const size_t copy_len = (ind.asdu_length < 16U) ? ind.asdu_length : 16U;
        (void)zb_trace_log_packet(ZB_DIR_RX, &meta, ind.asdu, copy_len);
        emit_event("DEVICE_COMMAND_ON_OFF");
    }
    return false;
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
        trace_meta_fill_neighbor_link(&meta);

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
        if (!device_table_has_short_addr(meta.src_short)) {
            ieee_resolve_schedule(meta.src_short);
        }
        bool reading_changed = false;
        if (m->status == ESP_ZB_ZCL_STATUS_SUCCESS) {
            reading_changed = zcl_apply_attribute_to_readings(meta.src_short, m->src_endpoint, m->cluster, &m->attribute);
            device_table_inc_counter("report_attr_ok");
            if (!reading_changed) {
                device_table_inc_counter("report_attr_unchanged");
            }
        }
        if (reading_changed) {
            emit_event("DEVICE_REPORT");
        }
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
        {
            int8_t nr = 0;
            uint8_t nl = 0;
            if (neighbor_table_lookup_link(meta.src_short, &nr, &nl)) {
                meta.lqi = nl;
                if (meta.rssi == 0) {
                    meta.rssi = nr;
                }
            }
        }
        const uint8_t payload[] = {m->info.header.fc, m->info.header.tsn, m->resp_to_cmd, (uint8_t)m->status_code};
        (void)zb_trace_log_packet(ZB_DIR_RX, &meta, payload, sizeof(payload));
        device_table_update_from_trace(&meta);
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *m = (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
        const uint16_t short_addr =
            (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) ? m->info.src_address.u.short_addr : 0xFFFF;
        zb_coordinator_note_inbound_device_traffic(short_addr);
        {
            int8_t r = m->info.header.rssi;
            uint8_t l = 0;
            int8_t nr = 0;
            uint8_t nl = 0;
            if (neighbor_table_lookup_link(short_addr, &nr, &nl)) {
                l = nl;
                if (r == 0) {
                    r = nr;
                }
            }
            device_table_update_rf_metrics(short_addr, r, l);
        }
        req_track_ack(false, short_addr, m->info.header.tsn);
        identity_job_t *job = identity_job_find(short_addr, m->info.src_endpoint);
        char manufacturer[DEVICE_TABLE_MAX_STR] = {0};
        char model[DEVICE_TABLE_MAX_STR] = {0};
        for (const esp_zb_zcl_read_attr_resp_variable_t *v = m->variables; v != NULL; v = v->next) {
            if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS || v->attribute.data.value == NULL) {
                device_table_inc_counter("read_rsp_fail");
                continue;
            }
            device_table_inc_counter("read_rsp_ok");
            zcl_apply_attribute_to_readings(short_addr, m->info.src_endpoint, m->info.cluster, &v->attribute);
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
        zb_coordinator_note_inbound_device_traffic(short_addr);
        req_track_ack(false, short_addr, m->info.header.tsn);
        const bool ok = (m->info.status == ESP_ZB_ZCL_STATUS_SUCCESS);
        device_table_mark_report_cfg(short_addr, ok, false);
        device_table_inc_counter(ok ? "report_cfg_rsp_ok" : "report_cfg_rsp_fail");
        ESP_LOGI(TAG, "[T+%07.3f] ConfigureReporting RSP short=0x%04X cluster=0x%04X status=0x%02X", timebase_now_s(), short_addr, m->info.cluster,
                 m->info.status);
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t zb_coordinator_init(zb_coordinator_event_cb_t event_cb)
{
    s_event_cb = event_cb;
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.version = ZB_COORD_RUNTIME_VERSION;

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
    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ZB_COORD_PRIMARY_CHANNEL_MASK));
    ESP_LOGI(TAG, "[T+%07.3f] Politica canales Zigbee mask=0x%08" PRIX32 " (%s)", timebase_now_s(), ZB_COORD_PRIMARY_CHANNEL_MASK,
             ZB_COORD_PRIMARY_CHANNEL_POLICY);

    esp_zb_aps_data_indication_handler_register(aps_data_indication_handler);
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
    pending_interview_drain();
    identity_jobs_poll();
    ieee_resolve_poll();
    health_and_interview_poll();
    refresh_runtime_from_stack();

    if (esp_zb_bdb_dev_joined()) {
        const TickType_t now_poll = xTaskGetTickCount();
        if (s_next_sensor_poll_tick == 0) {
            s_next_sensor_poll_tick = now_poll + pdMS_TO_TICKS(SENSOR_POLL_PERIOD_MS);
        } else if ((int32_t)(now_poll - s_next_sensor_poll_tick) >= 0) {
            s_next_sensor_poll_tick = now_poll + pdMS_TO_TICKS(SENSOR_POLL_PERIOD_MS);
            device_table_request_sensor_poll_reads(sensor_poll_issue_read);
        }
    } else {
        s_next_sensor_poll_tick = 0;
    }

    esp_zb_lock_release();
    return ESP_OK;
}

esp_err_t zb_coordinator_get_runtime_state(zb_network_runtime_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_state = s_runtime;
    return ESP_OK;
}

esp_err_t zb_coordinator_local_reset(void)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(250))) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "[T+%07.3f] LOCAL_RESET solicitado: leave + borrado persistencia Zigbee oficial", timebase_now_s());
    esp_zb_bdb_reset_via_local_action();
    esp_zb_lock_release();
    return ESP_OK;
}

void zb_coordinator_factory_reset(void)
{
    if (!s_stack_started) {
        ESP_LOGW(TAG, "[T+%07.3f] FACTORY_RESET solicitado sin stack Zigbee activo; reinicio MCU", timebase_now_s());
        esp_restart();
        return;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(250))) {
        ESP_LOGW(TAG, "[T+%07.3f] FACTORY_RESET sin lock Zigbee; reinicio MCU simple", timebase_now_s());
        esp_restart();
        return;
    }
    ESP_LOGW(TAG, "[T+%07.3f] FACTORY_RESET solicitado: borrado completo de zb_storage y reinicio", timebase_now_s());
    esp_zb_factory_reset();
    esp_zb_lock_release();
}

void zb_coordinator_get_ram_snapshot(zb_coordinator_ram_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->runtime = s_runtime;

    for (size_t i = 0; i < INTERVIEW_JOB_MAX && out->interview_count < ZB_COORD_MAX_INTERVIEW_DUMP; ++i) {
        const interview_job_t *j = &s_interview_jobs[i];
        if (!j->used) {
            continue;
        }
        zb_coord_interview_dump_t *d = &out->interviews[out->interview_count++];
        d->used = j->used;
        d->short_addr = j->short_addr;
        d->endpoint = j->endpoint;
        d->phase = j->phase;
        d->retries = j->retries;
        d->ep_count = j->ep_count;
        d->ep_idx = j->ep_idx;
        d->node_desc_ok = j->node_desc_ok;
        d->active_ep_ok = j->active_ep_ok;
        d->simple_desc_ok = j->simple_desc_ok;
        d->last_seen_s = j->last_seen_s;
        d->last_interview_s = j->last_interview_s;
    }

    for (size_t i = 0; i < IDENTITY_JOB_MAX && out->identity_count < ZB_COORD_MAX_IDENTITY_DUMP; ++i) {
        const identity_job_t *j = &s_identity_jobs[i];
        if (!j->used) {
            continue;
        }
        zb_coord_identity_dump_t *d = &out->identities[out->identity_count++];
        d->used = j->used;
        d->short_addr = j->short_addr;
        d->endpoint = j->endpoint;
        d->attempts = j->attempts;
        d->got_manufacturer = j->got_manufacturer;
        d->got_model = j->got_model;
    }
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
            log_network_runtime_summary("restaurada");
            emit_event("NETWORK_RESTORED");
            recontact_known_devices();
        }
        return;
    }

    if (sig == ESP_ZB_BDB_SIGNAL_FORMATION && st == ESP_OK) {
        log_network_runtime_summary("formada");
        emit_event("NETWORK_FORMED");
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE) {
        const esp_zb_zdo_signal_device_annce_params_t *p =
            (const esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            const uint64_t ieee = ieee_to_u64(p->ieee_addr);
            device_table_update_discovery(ieee, p->device_short_addr, 0, NULL, NULL);
            device_table_inc_counter("device_announce");

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
            (void)interview_request_enqueue(p->device_short_addr, "device_announce");
        }
        emit_event("DEVICE_ANNOUNCE");
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE) {
        const esp_zb_zdo_signal_device_update_params_t *p =
            (const esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            const uint64_t ieee = ieee_to_u64(p->long_addr);
            device_table_update_device_update(ieee, p->short_addr, p->parent_short, p->status, p->tc_action);
            zb_coordinator_note_inbound_device_traffic(p->short_addr);
            device_table_inc_counter("device_update");
            ESP_LOGI(TAG, "[T+%07.3f] DeviceUpdate short=0x%04X status=%u tc_action=%u parent=0x%04X", timebase_now_s(), p->short_addr,
                     (unsigned)p->status, (unsigned)p->tc_action, p->parent_short);
            if (p->status == ESP_ZB_ZDO_STANDARD_DEV_SECURED_REJOIN || p->status == ESP_ZB_ZDO_STANDARD_DEV_UNSECURED_JOIN ||
                p->status == ESP_ZB_ZDO_STANDARD_DEV_TC_REJOIN) {
                device_table_inc_counter("device_rejoin");
                /* Esperar a DEVICE_ANNCE o autorizacion OK para evitar entrevistas sobre rejoins rechazados. */
            }
            emit_event("DEVICE_UPDATE");
        }
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED) {
        const esp_zb_zdo_signal_device_authorized_params_t *p =
            (const esp_zb_zdo_signal_device_authorized_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            device_table_update_authorization(ieee_to_u64(p->long_addr), p->short_addr, p->authorization_type, p->authorization_status);
            if (p->authorization_status == 0U) {
                ESP_LOGI(TAG, "[T+%07.3f] DeviceAuthorized OK short=0x%04X type=%u", timebase_now_s(), p->short_addr,
                         (unsigned)p->authorization_type);
                emit_event("DEVICE_AUTHORIZED");
                (void)interview_request_enqueue(p->short_addr, "device_authorized");
            } else {
                ESP_LOGW(TAG,
                         "[T+%07.3f] DeviceAuthorized FAIL short=0x%04X type=%u auth_status=%u; probable rejoin a red antigua, resetear el dispositivo final",
                         timebase_now_s(), p->short_addr, (unsigned)p->authorization_type, (unsigned)p->authorization_status);
                interview_cancel(p->short_addr, "authorization_failed");
                emit_event("DEVICE_AUTH_FAILED");
            }
        }
        return;
    }

    if (sig == ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION) {
        const esp_zb_zdo_signal_leave_indication_params_t *p =
            (const esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(signal_s->p_app_signal);
        if (p != NULL) {
            device_table_mark_leave(ieee_to_u64(p->device_addr), p->short_addr, p->rejoin != 0U);
        }
        emit_event("DEVICE_LEAVE");
        return;
    }
}

