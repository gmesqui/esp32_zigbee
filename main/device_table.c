#include "device_table.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "timebase.h"

static const char *TAG = "device_table";
static device_record_t s_devices[DEVICE_TABLE_MAX_DEVICES];
static device_table_telemetry_t s_telemetry;
static const char *CACHE_NS = "zb_cache";
static const char *CACHE_KEY_LEGACY = "devtab_v2";
#define DEVICE_TABLE_MAX_ROUTE_SUMMARY 32
#define CACHE_VER 8U
#define CACHE_CHUNK_SLOTS 8
#define CACHE_NUM_CHUNKS (DEVICE_TABLE_MAX_DEVICES / CACHE_CHUNK_SLOTS)
#define CACHE_FLUSH_DEBOUNCE_S 1.5
#define SENSOR_POLL_FRESH_S 300.0

_Static_assert(DEVICE_TABLE_MAX_DEVICES % CACHE_CHUNK_SLOTS == 0, "DEVICE_TABLE_MAX_DEVICES debe ser multiplo de CACHE_CHUNK_SLOTS");
static bool s_cache_loaded = false;
static char s_cache_nvs_format[20] = "none";
static bool s_cache_dirty = false;
static double s_cache_last_dirty_s = 0.0;
static uint32_t s_cache_flash_write_count = 0;
static uint32_t s_cache_dirty_generation = 0;
static SemaphoreHandle_t s_device_table_mutex = NULL;
static void update_norm_type(device_record_t *rec);
static bool push_unique_u16(uint16_t *arr, size_t *len, size_t max, uint16_t v);
static device_endpoint_record_t *find_endpoint(device_record_t *rec, uint8_t endpoint_id);
static device_endpoint_record_t *get_or_create_endpoint(device_record_t *rec, uint8_t endpoint_id);

static bool device_table_lock(void)
{
    return (s_device_table_mutex == NULL) || (xSemaphoreTakeRecursive(s_device_table_mutex, portMAX_DELAY) == pdTRUE);
}

static void device_table_unlock(void)
{
    if (s_device_table_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_device_table_mutex);
    }
}

typedef struct {
    bool used;
    uint8_t endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t device_version;
    uint16_t input_clusters[DEVICE_TABLE_MAX_CLUSTERS];
    uint8_t input_clusters_len;
    uint16_t output_clusters[DEVICE_TABLE_MAX_CLUSTERS];
    uint8_t output_clusters_len;
} device_cache_endpoint_record_t;

typedef struct {
    bool occupied;
    uint64_t ieee;
    char manufacturer[DEVICE_TABLE_MAX_STR];
    char model[DEVICE_TABLE_MAX_STR];
    uint8_t endpoint_count;
    device_cache_endpoint_record_t endpoints[DEVICE_TABLE_MAX_ENDPOINTS];
} device_cache_record_t;

typedef struct {
    uint32_t version;
    uint32_t device_count;
    uint32_t device_slots;
    uint32_t record_size;
    uint32_t chunk_slots;
    uint32_t chunk_size;
} device_table_cache_head_t;

static device_cache_record_t s_chunk_scratch[CACHE_CHUNK_SLOTS];

_Static_assert(sizeof(device_cache_endpoint_record_t) <= 80U, "cache endpoint inesperadamente grande");
_Static_assert(sizeof(device_cache_record_t) <= 704U, "cache record inesperadamente grande");

static void mark_cache_dirty(void)
{
    s_cache_dirty = true;
    s_cache_last_dirty_s = timebase_now_s();
    s_cache_dirty_generation++;
}

static void clear_device_record(device_record_t *rec)
{
    if (rec == NULL) {
        return;
    }
    memset(rec, 0, sizeof(*rec));
}

static void merge_endpoint_record(device_endpoint_record_t *dst, const device_endpoint_record_t *src)
{
    if (dst == NULL || src == NULL || !src->used) {
        return;
    }
    if (!dst->used) {
        *dst = *src;
        return;
    }
    if (dst->profile_id == 0U) {
        dst->profile_id = src->profile_id;
    }
    if (dst->device_id == 0U) {
        dst->device_id = src->device_id;
    }
    if (dst->device_version == 0U) {
        dst->device_version = src->device_version;
    }
    for (size_t i = 0; i < src->input_clusters_len; ++i) {
        (void)push_unique_u16(dst->input_clusters, &dst->input_clusters_len, DEVICE_TABLE_MAX_CLUSTERS, src->input_clusters[i]);
    }
    for (size_t i = 0; i < src->output_clusters_len; ++i) {
        (void)push_unique_u16(dst->output_clusters, &dst->output_clusters_len, DEVICE_TABLE_MAX_CLUSTERS, src->output_clusters[i]);
    }
}

static void merge_device_record(device_record_t *dst, const device_record_t *src)
{
    if (dst == NULL || src == NULL || !src->occupied) {
        return;
    }
    if (!dst->occupied) {
        *dst = *src;
        return;
    }
    dst->in_network |= src->in_network;
    dst->seen_in_device_annce |= src->seen_in_device_annce;
    dst->authorized |= src->authorized;
    if (dst->authorization_type == 0U) {
        dst->authorization_type = src->authorization_type;
    }
    if (dst->authorization_status == 0U) {
        dst->authorization_status = src->authorization_status;
    }
    if (dst->short_addr == 0x0000U || dst->short_addr == 0xFFFFU) {
        dst->short_addr = src->short_addr;
    }
    if (src->last_seen_s > dst->last_seen_s) {
        dst->last_seen_s = src->last_seen_s;
        dst->rssi = src->rssi;
        dst->lqi = src->lqi;
    }
    if (dst->manufacturer[0] == '\0' && src->manufacturer[0] != '\0') {
        snprintf(dst->manufacturer, sizeof(dst->manufacturer), "%s", src->manufacturer);
    }
    if (dst->model[0] == '\0' && src->model[0] != '\0') {
        snprintf(dst->model, sizeof(dst->model), "%s", src->model);
    }
    if (dst->parent_short == 0x0000U || dst->parent_short == 0xFFFFU) {
        dst->parent_short = src->parent_short;
    }
    if (dst->update_status == 0U) {
        dst->update_status = src->update_status;
    }
    if (dst->tc_action == 0U) {
        dst->tc_action = src->tc_action;
    }
    if (dst->node_desc_flags == 0U) {
        dst->node_desc_flags = src->node_desc_flags;
        dst->mac_capability_flags = src->mac_capability_flags;
        dst->manufacturer_code = src->manufacturer_code;
        dst->max_buf_size = src->max_buf_size;
        dst->max_incoming_transfer_size = src->max_incoming_transfer_size;
        dst->server_mask = src->server_mask;
        dst->max_outgoing_transfer_size = src->max_outgoing_transfer_size;
        dst->desc_capability_field = src->desc_capability_field;
    }
    if (src->interview_phase > dst->interview_phase) {
        dst->interview_phase = src->interview_phase;
    }
    if (src->interview_retries > dst->interview_retries) {
        dst->interview_retries = src->interview_retries;
    }
    if (src->report_cfg_retries > dst->report_cfg_retries) {
        dst->report_cfg_retries = src->report_cfg_retries;
    }
    dst->report_cfg_ok |= src->report_cfg_ok;
    dst->silent |= src->silent;
    if (src->silence_level > dst->silence_level) {
        dst->silence_level = src->silence_level;
    }
    if (src->last_interview_s > dst->last_interview_s) {
        dst->last_interview_s = src->last_interview_s;
    }
    if (src->last_report_cfg_s > dst->last_report_cfg_s) {
        dst->last_report_cfg_s = src->last_report_cfg_s;
    }
    if (dst->norm_type == DEVICE_NORM_UNKNOWN) {
        dst->norm_type = src->norm_type;
        snprintf(dst->norm_name, sizeof(dst->norm_name), "%s", src->norm_name);
    }

    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t *src_ep = &src->endpoints[i];
        if (!src_ep->used) {
            continue;
        }
        device_endpoint_record_t *dst_ep = find_endpoint(dst, src_ep->endpoint_id);
        if (dst_ep == NULL) {
            dst_ep = get_or_create_endpoint(dst, src_ep->endpoint_id);
        }
        merge_endpoint_record(dst_ep, src_ep);
    }
    update_norm_type(dst);
}

static bool canonicalize_device_records(void)
{
    bool changed = false;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        device_record_t *a = &s_devices[i];
        if (!a->occupied || a->ieee == 0U) {
            continue;
        }
        for (size_t j = i + 1; j < DEVICE_TABLE_MAX_DEVICES; ++j) {
            device_record_t *b = &s_devices[j];
            if (!b->occupied || b->ieee != a->ieee) {
                continue;
            }
            merge_device_record(a, b);
            clear_device_record(b);
            changed = true;
        }
    }
    if (changed) {
        ESP_LOGW(TAG, "[T+%07.3f] Tabla dispositivos normalizada: duplicados por IEEE fusionados", timebase_now_s());
        mark_cache_dirty();
    }
    return changed;
}

static size_t count_occupied_devices(void)
{
    size_t n = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied) {
            n++;
        }
    }
    return n;
}

static size_t cache_payload_reserved_bytes(void)
{
    return sizeof(device_table_cache_head_t) + sizeof(s_chunk_scratch) * CACHE_NUM_CHUNKS;
}

static void log_devices_table_contents(const char *prefix)
{
    const size_t occ = count_occupied_devices();
    ESP_LOGI(TAG,
             "%s tabla RAM: ocupados=%u/%u sizeof(device_record_t)=%u sizeof(cache_record_t)=%u cache_reservada_max=%u B",
             prefix, (unsigned)occ, (unsigned)DEVICE_TABLE_MAX_DEVICES, (unsigned)sizeof(device_record_t), (unsigned)sizeof(device_cache_record_t),
             (unsigned)cache_payload_reserved_bytes());
    unsigned logged = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES && logged < 16U; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
            continue;
        }
        ESP_LOGI(TAG, "%s  [%u] ieee=0x%016" PRIX64 " short=0x%04X in_network=%s eps=%u mfg='%s' model='%s' last_seen=%.3f", prefix,
                 (unsigned)i, d->ieee, d->short_addr, d->in_network ? "true" : "false", (unsigned)d->endpoint_count, d->manufacturer, d->model,
                 d->last_seen_s);
        logged++;
    }
    if (occ > 16U) {
        ESP_LOGI(TAG, "%s  ... y %u dispositivos mas (truncado en log)", prefix, (unsigned)(occ - 16U));
    }
}

static void cache_chunk_key(char *out, size_t out_sz, unsigned chunk_idx)
{
    (void)snprintf(out, out_sz, "dt2_d%u", chunk_idx);
}

static void cache_record_from_device(device_cache_record_t *dst, const device_record_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src == NULL || !src->occupied) {
        return;
    }

    dst->occupied = true;
    dst->ieee = src->ieee;
    snprintf(dst->manufacturer, sizeof(dst->manufacturer), "%s", src->manufacturer);
    snprintf(dst->model, sizeof(dst->model), "%s", src->model);
    dst->endpoint_count = (uint8_t)src->endpoint_count;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t *src_ep = &src->endpoints[i];
        device_cache_endpoint_record_t *dst_ep = &dst->endpoints[i];
        if (!src_ep->used) {
            continue;
        }
        dst_ep->used = true;
        dst_ep->endpoint_id = src_ep->endpoint_id;
        dst_ep->profile_id = src_ep->profile_id;
        dst_ep->device_id = src_ep->device_id;
        dst_ep->device_version = src_ep->device_version;
        dst_ep->input_clusters_len = (uint8_t)src_ep->input_clusters_len;
        dst_ep->output_clusters_len = (uint8_t)src_ep->output_clusters_len;
        memcpy(dst_ep->input_clusters, src_ep->input_clusters, sizeof(dst_ep->input_clusters));
        memcpy(dst_ep->output_clusters, src_ep->output_clusters, sizeof(dst_ep->output_clusters));
    }
}

static void device_from_cache_record(device_record_t *dst, const device_cache_record_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src == NULL || !src->occupied) {
        return;
    }

    dst->occupied = true;
    dst->ieee = src->ieee;
    snprintf(dst->manufacturer, sizeof(dst->manufacturer), "%s", src->manufacturer);
    snprintf(dst->model, sizeof(dst->model), "%s", src->model);
    dst->endpoint_count = src->endpoint_count;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_cache_endpoint_record_t *src_ep = &src->endpoints[i];
        device_endpoint_record_t *dst_ep = &dst->endpoints[i];
        if (!src_ep->used) {
            continue;
        }
        dst_ep->used = true;
        dst_ep->endpoint_id = src_ep->endpoint_id;
        dst_ep->profile_id = src_ep->profile_id;
        dst_ep->device_id = src_ep->device_id;
        dst_ep->device_version = src_ep->device_version;
        dst_ep->input_clusters_len = src_ep->input_clusters_len;
        dst_ep->output_clusters_len = src_ep->output_clusters_len;
        memcpy(dst_ep->input_clusters, src_ep->input_clusters, sizeof(dst_ep->input_clusters));
        memcpy(dst_ep->output_clusters, src_ep->output_clusters, sizeof(dst_ep->output_clusters));
    }
    update_norm_type(dst);
}

static bool load_cache_chunked(nvs_handle_t nvs)
{
    device_table_cache_head_t head;
    size_t len = sizeof(head);
    esp_err_t err = nvs_get_blob(nvs, "dt2_head", &head, &len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS dt2_head: %s (chunked no presente)", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(head)) {
        ESP_LOGW(TAG, "NVS dt2_head tamano incoherente: leido=%u esperado=%u", (unsigned)len, (unsigned)sizeof(head));
        return false;
    }
    if (head.version != CACHE_VER) {
        ESP_LOGW(TAG, "NVS dt2_head version=%" PRIu32 " no soportada (esperado %u)", head.version, (unsigned)CACHE_VER);
        return false;
    }
    if (head.device_slots != DEVICE_TABLE_MAX_DEVICES || head.record_size != sizeof(device_cache_record_t) ||
        head.chunk_slots != CACHE_CHUNK_SLOTS || head.chunk_size != sizeof(s_chunk_scratch)) {
        ESP_LOGW(TAG,
                 "NVS dt2_head incompatible: slots=%" PRIu32 "/%u record=%" PRIu32 "/%u chunk_slots=%" PRIu32 "/%u chunk=%" PRIu32 "/%u",
                 head.device_slots, (unsigned)DEVICE_TABLE_MAX_DEVICES, head.record_size, (unsigned)sizeof(device_cache_record_t), head.chunk_slots,
                 (unsigned)CACHE_CHUNK_SLOTS, head.chunk_size, (unsigned)sizeof(s_chunk_scratch));
        return false;
    }

    memset(s_devices, 0, sizeof(s_devices));
    for (size_t c = 0; c < CACHE_NUM_CHUNKS; ++c) {
        char key[16];
        cache_chunk_key(key, sizeof(key), (unsigned)c);
        memset(s_chunk_scratch, 0, sizeof(s_chunk_scratch));
        len = sizeof(s_chunk_scratch);
        err = nvs_get_blob(nvs, key, &s_chunk_scratch[0], &len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS clave '%s' fallo: %s", key, esp_err_to_name(err));
            return false;
        }
        if (len != sizeof(s_chunk_scratch)) {
            ESP_LOGW(TAG, "NVS '%s' tamano leido=%u esperado=%u", key, (unsigned)len, (unsigned)sizeof(s_chunk_scratch));
            return false;
        }
        for (size_t i = 0; i < CACHE_CHUNK_SLOTS; ++i) {
            device_from_cache_record(&s_devices[(c * CACHE_CHUNK_SLOTS) + i], &s_chunk_scratch[i]);
        }
    }

    s_cache_loaded = true;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "chunked-fixed");
    (void)canonicalize_device_records();
    ESP_LOGI(TAG,
             "[T+%07.3f] NVS cache minima cargada formato=%s head=%u B + %u x %u B (reservado_max=%u B, devices=%" PRIu32 ")",
             timebase_now_s(), s_cache_nvs_format, (unsigned)sizeof(device_table_cache_head_t), (unsigned)CACHE_NUM_CHUNKS,
             (unsigned)sizeof(s_chunk_scratch), (unsigned)cache_payload_reserved_bytes(), head.device_count);
    log_devices_table_contents("NVS leido");
    return true;
}

static device_record_t *find_by_ieee(uint64_t ieee)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].ieee == ieee) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_record_t *find_by_short(uint16_t short_addr)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_record_t *pick_slot(void)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!s_devices[i].occupied) {
            return &s_devices[i];
        }
    }

    size_t oldest_idx = 0;
    double oldest = s_devices[0].last_seen_s;
    for (size_t i = 1; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].last_seen_s < oldest) {
            oldest = s_devices[i].last_seen_s;
            oldest_idx = i;
        }
    }
    return &s_devices[oldest_idx];
}

static device_record_t *ensure_device(uint64_t ieee)
{
    device_record_t *rec = find_by_ieee(ieee);
    if (rec != NULL) {
        return rec;
    }
    rec = pick_slot();
    memset(rec, 0, sizeof(*rec));
    rec->occupied = true;
    rec->in_network = true;
    rec->ieee = ieee;
    return rec;
}

static device_endpoint_record_t *find_endpoint(device_record_t *rec, uint8_t endpoint_id)
{
    if (rec == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < rec->endpoint_count; ++i) {
        if (rec->endpoints[i].used && rec->endpoints[i].endpoint_id == endpoint_id) {
            return &rec->endpoints[i];
        }
    }
    return NULL;
}

static device_endpoint_record_t *get_or_create_endpoint(device_record_t *rec, uint8_t endpoint_id)
{
    device_endpoint_record_t *ep = find_endpoint(rec, endpoint_id);
    if (ep != NULL) {
        return ep;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        if (!rec->endpoints[i].used) {
            ep = &rec->endpoints[i];
            memset(ep, 0, sizeof(*ep));
            ep->used = true;
            ep->endpoint_id = endpoint_id;
            if (i >= rec->endpoint_count) {
                rec->endpoint_count = i + 1U;
            }
            return ep;
        }
    }
    return NULL;
}

static bool push_unique_u16(uint16_t *arr, size_t *len, size_t max, uint16_t v)
{
    for (size_t i = 0; i < *len; ++i) {
        if (arr[i] == v) {
            return false;
        }
    }
    if (*len >= max) {
        return false;
    }
    arr[*len] = v;
    (*len)++;
    return true;
}

static bool endpoint_has_cluster(const device_endpoint_record_t *ep, uint16_t cluster_id)
{
    if (ep == NULL) {
        return false;
    }
    for (size_t i = 0; i < ep->input_clusters_len; ++i) {
        if (ep->input_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static const char *norm_name(device_norm_t t)
{
    switch (t) {
    case DEVICE_NORM_SWITCH:
        return "switch";
    case DEVICE_NORM_TEMP_HUMIDITY:
        return "temp_humidity";
    case DEVICE_NORM_PRESENCE:
        return "presence";
    case DEVICE_NORM_TEMP:
        return "temperature";
    case DEVICE_NORM_UNKNOWN:
    default:
        return "unknown";
    }
}

static void update_norm_type(device_record_t *rec)
{
    bool onoff = false;
    bool temp = false;
    bool hum = false;
    bool occ = false;

    if (rec == NULL) {
        return;
    }

    for (size_t e = 0; e < DEVICE_TABLE_MAX_ENDPOINTS; ++e) {
        const device_endpoint_record_t *ep = &rec->endpoints[e];
        if (!ep->used) {
            continue;
        }
        for (size_t i = 0; i < ep->input_clusters_len; ++i) {
            const uint16_t c = ep->input_clusters[i];
            onoff |= (c == 0x0006U);
            temp |= (c == 0x0402U);
            hum |= (c == 0x0405U);
            occ |= (c == 0x0406U || c == 0x0500U);
        }
    }

    if (onoff) {
        rec->norm_type = DEVICE_NORM_SWITCH;
    } else if (temp && hum) {
        rec->norm_type = DEVICE_NORM_TEMP_HUMIDITY;
    } else if (occ) {
        rec->norm_type = DEVICE_NORM_PRESENCE;
    } else if (temp) {
        rec->norm_type = DEVICE_NORM_TEMP;
    } else {
        rec->norm_type = DEVICE_NORM_UNKNOWN;
    }
    snprintf(rec->norm_name, sizeof(rec->norm_name), "%s", norm_name(rec->norm_type));
}

void device_table_init(void)
{
    if (s_device_table_mutex == NULL) {
        s_device_table_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_device_table_mutex == NULL) {
            ESP_LOGE(TAG, "[T+%07.3f] No se pudo crear mutex de device_table; se continua sin proteccion", timebase_now_s());
        }
    }
    if (!device_table_lock()) {
        ESP_LOGE(TAG, "[T+%07.3f] No se pudo bloquear device_table_init", timebase_now_s());
        return;
    }
    memset(s_devices, 0, sizeof(s_devices));
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    s_cache_loaded = false;
    s_cache_dirty = false;
    s_cache_last_dirty_s = 0.0;
    s_cache_dirty_generation = 0;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "none");
    ESP_LOGI(TAG, "[T+%07.3f] Cache minima NVS: record=%u B chunk=%u B reservado_max=%u B slots=%u", timebase_now_s(),
             (unsigned)sizeof(device_cache_record_t), (unsigned)sizeof(s_chunk_scratch), (unsigned)cache_payload_reserved_bytes(),
             (unsigned)DEVICE_TABLE_MAX_DEVICES);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CACHE_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS namespace '%s' inexistente (primer arranque o sin guardados previos)", timebase_now_s(), CACHE_NS);
        device_table_unlock();
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] NVS nvs_open RO '%s' fallo: %s", timebase_now_s(), CACHE_NS, esp_err_to_name(err));
        device_table_unlock();
        return;
    }

    if (!load_cache_chunked(nvs)) {
        ESP_LOGI(TAG, "[T+%07.3f] NVS sin cache minima valida", timebase_now_s());
    }
    nvs_close(nvs);
    device_table_unlock();
}

void device_table_clear_cache_and_runtime(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CACHE_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        for (size_t c = 0; c < CACHE_NUM_CHUNKS; ++c) {
            char key[16];
            cache_chunk_key(key, sizeof(key), (unsigned)c);
            esp_err_t er = nvs_erase_key(nvs, key);
            if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "[T+%07.3f] NVS borrar '%s' fallo: %s", timebase_now_s(), key, esp_err_to_name(er));
            }
        }
        {
            esp_err_t er = nvs_erase_key(nvs, "dt2_head");
            if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "[T+%07.3f] NVS borrar 'dt2_head' fallo: %s", timebase_now_s(), esp_err_to_name(er));
            }
        }
        {
            esp_err_t er = nvs_erase_key(nvs, CACHE_KEY_LEGACY);
            if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "[T+%07.3f] NVS borrar legacy '%s' fallo: %s", timebase_now_s(), CACHE_KEY_LEGACY, esp_err_to_name(er));
            }
        }
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[T+%07.3f] NVS commit borrado zb_cache fallo: %s", timebase_now_s(), esp_err_to_name(err));
        }
        nvs_close(nvs);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS open '%s' para borrado fallo: %s", timebase_now_s(), CACHE_NS, esp_err_to_name(err));
    }

    if (!device_table_lock()) {
        ESP_LOGE(TAG, "[T+%07.3f] No se pudo bloquear device_table_clear_cache_and_runtime", timebase_now_s());
        return;
    }
    memset(s_devices, 0, sizeof(s_devices));
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    s_cache_loaded = false;
    s_cache_dirty = false;
    s_cache_last_dirty_s = 0.0;
    s_cache_flash_write_count = 0;
    s_cache_dirty_generation = 0;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "none");
    device_table_unlock();
    ESP_LOGW(TAG, "[T+%07.3f] Cache zb_cache borrada y estado RAM reiniciado", timebase_now_s());
}

void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi)
{
    if (!device_table_lock()) {
        return;
    }
    const bool existed = (find_by_ieee(ieee) != NULL);
    device_record_t *rec = ensure_device(ieee);
    rec->in_network = true;
    rec->short_addr = short_addr;
    rec->rssi = rssi;
    rec->lqi = lqi;
    rec->last_seen_s = timebase_now_s();
    if (!existed) {
        mark_cache_dirty();
    }
    device_table_unlock();
}

void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model)
{
    if (!device_table_lock()) {
        return;
    }
    const bool existed = (find_by_ieee(ieee) != NULL);
    device_record_t *rec = ensure_device(ieee);
    bool persist_changed = !existed;
    rec->in_network = true;
    rec->seen_in_device_annce = true;
    rec->short_addr = short_addr;
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL) {
        persist_changed |= (strncmp(rec->manufacturer, manufacturer, sizeof(rec->manufacturer)) != 0);
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL) {
        persist_changed |= (strncmp(rec->model, model, sizeof(rec->model)) != 0);
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
    if (device_id != 0U) {
        device_endpoint_record_t *ep = get_or_create_endpoint(rec, 1U);
        if (ep != NULL) {
            persist_changed |= (!ep->used || ep->device_id != device_id);
            ep->device_id = device_id;
        }
    }
    update_norm_type(rec);
    if (persist_changed) {
        mark_cache_dirty();
    }
    device_table_unlock();
}

void device_table_update_from_trace(const zb_trace_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(meta->src_short);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->rssi = meta->rssi;
    rec->lqi = meta->lqi;
    rec->last_seen_s = timebase_now_s();
    device_table_unlock();
}

void device_table_update_rf_metrics(uint16_t short_addr, int8_t rssi, uint8_t lqi)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->rssi = rssi;
    rec->lqi = lqi;
    rec->last_seen_s = timebase_now_s();
    device_table_unlock();
}

void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    bool persist_changed = false;
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL && manufacturer[0] != '\0') {
        persist_changed |= (strncmp(rec->manufacturer, manufacturer, sizeof(rec->manufacturer)) != 0);
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL && model[0] != '\0') {
        persist_changed |= (strncmp(rec->model, model, sizeof(rec->model)) != 0);
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
    if (persist_changed) {
        mark_cache_dirty();
    }
    device_table_unlock();
}

void device_table_update_node_desc(uint16_t short_addr, uint16_t node_desc_flags, uint8_t mac_capability_flags, uint16_t manufacturer_code,
                                   uint8_t max_buf_size, uint16_t max_incoming_transfer_size, uint16_t server_mask,
                                   uint16_t max_outgoing_transfer_size, uint8_t desc_capability_field)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->last_seen_s = timebase_now_s();
    rec->node_desc_flags = node_desc_flags;
    rec->mac_capability_flags = mac_capability_flags;
    rec->manufacturer_code = manufacturer_code;
    rec->max_buf_size = max_buf_size;
    rec->max_incoming_transfer_size = max_incoming_transfer_size;
    rec->server_mask = server_mask;
    rec->max_outgoing_transfer_size = max_outgoing_transfer_size;
    rec->desc_capability_field = desc_capability_field;
    device_table_unlock();
}

void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t profile_id, uint16_t device_id, uint8_t device_version,
                                     const uint16_t *clusters_in, size_t clusters_in_len, const uint16_t *clusters_out,
                                     size_t clusters_out_len)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }

    device_endpoint_record_t *ep = get_or_create_endpoint(rec, endpoint);
    if (ep == NULL) {
        device_table_unlock();
        return;
    }

    ep->profile_id = profile_id;
    ep->device_id = device_id;
    ep->device_version = device_version;
    ep->input_clusters_len = 0;
    ep->output_clusters_len = 0;
    if (clusters_in != NULL) {
        for (size_t i = 0; i < clusters_in_len; ++i) {
            (void)push_unique_u16(ep->input_clusters, &ep->input_clusters_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_in[i]);
        }
    }
    if (clusters_out != NULL) {
        for (size_t i = 0; i < clusters_out_len; ++i) {
            (void)push_unique_u16(ep->output_clusters, &ep->output_clusters_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_out[i]);
        }
    }
    rec->last_seen_s = timebase_now_s();
    update_norm_type(rec);
    mark_cache_dirty();
    device_table_unlock();
}

void device_table_update_device_update(uint64_t ieee, uint16_t short_addr, uint16_t parent_short, uint8_t status, uint8_t tc_action)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = ensure_device(ieee);
    rec->in_network = true;
    rec->short_addr = short_addr;
    rec->parent_short = parent_short;
    rec->update_status = status;
    rec->tc_action = tc_action;
    rec->last_seen_s = timebase_now_s();
    device_table_unlock();
}

void device_table_update_authorization(uint64_t ieee, uint16_t short_addr, uint8_t authorization_type, uint8_t authorization_status)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = ensure_device(ieee);
    rec->in_network = true;
    rec->short_addr = short_addr;
    rec->authorized = (authorization_status == 0U);
    rec->authorization_type = authorization_type;
    rec->authorization_status = authorization_status;
    rec->last_seen_s = timebase_now_s();
    device_table_unlock();
}

void device_table_mark_leave(uint64_t ieee, uint16_t short_addr, bool rejoin)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = NULL;
    if (ieee != 0U) {
        rec = find_by_ieee(ieee);
    }
    if (rec == NULL && short_addr != 0x0000U && short_addr != 0xFFFFU) {
        rec = find_by_short(short_addr);
    }
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->in_network = rejoin;
    rec->seen_in_device_annce = false;
    rec->authorized = false;
    rec->silent = !rejoin;
    rec->silence_level = rejoin ? 1U : 2U;
    rec->last_seen_s = timebase_now_s();
    device_table_unlock();
}

void device_table_mark_interview(uint16_t short_addr, uint8_t phase, bool success, bool is_retry)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->interview_phase = phase;
    rec->last_interview_s = timebase_now_s();
    if (is_retry) {
        rec->interview_retries++;
        s_telemetry.interview_retries++;
    }
    if (phase == 1U) {
        s_telemetry.interview_started++;
    }
    if (success) {
        s_telemetry.interview_completed++;
    } else if (phase == 0xFFU) {
        s_telemetry.interview_failed++;
    }
    device_table_unlock();
}

void device_table_mark_report_cfg(uint16_t short_addr, bool success, bool is_retry)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->last_report_cfg_s = timebase_now_s();
    if (is_retry) {
        rec->report_cfg_retries++;
    }
    rec->report_cfg_ok = success;
    device_table_unlock();
}

void device_table_mark_silent(uint16_t short_addr, bool silent)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    rec->silent = silent;
    if (!silent) {
        rec->silence_level = 0;
    } else if (rec->silence_level == 0) {
        rec->silence_level = 1;
    }
    device_table_unlock();
}

void device_table_mark_absent_prolonged(uint16_t short_addr, bool absent)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    if (absent) {
        rec->silent = true;
        rec->silence_level = 2;
    } else if (rec->silence_level == 2) {
        rec->silence_level = 1;
    }
    device_table_unlock();
}

void device_table_note_latency(bool is_zdo, double latency_ms)
{
    if (latency_ms < 0.0) {
        return;
    }
    if (!device_table_lock()) {
        return;
    }
    if (is_zdo) {
        s_telemetry.zdo_latency_avg_ms =
            ((s_telemetry.zdo_latency_avg_ms * s_telemetry.zdo_latency_samples) + latency_ms) / (s_telemetry.zdo_latency_samples + 1U);
        s_telemetry.zdo_latency_samples++;
    } else {
        s_telemetry.zcl_latency_avg_ms =
            ((s_telemetry.zcl_latency_avg_ms * s_telemetry.zcl_latency_samples) + latency_ms) / (s_telemetry.zcl_latency_samples + 1U);
        s_telemetry.zcl_latency_samples++;
    }
    device_table_unlock();
}

void device_table_inc_counter(const char *counter_name)
{
    if (counter_name == NULL) {
        return;
    }
    if (!device_table_lock()) {
        return;
    }
    if (strcmp(counter_name, "device_announce") == 0) {
        s_telemetry.device_announce++;
    } else if (strcmp(counter_name, "device_update") == 0) {
        s_telemetry.device_update++;
    } else if (strcmp(counter_name, "device_rejoin") == 0) {
        s_telemetry.device_rejoin++;
    } else if (strcmp(counter_name, "read_req") == 0) {
        s_telemetry.read_req++;
    } else if (strcmp(counter_name, "read_rsp_ok") == 0) {
        s_telemetry.read_rsp_ok++;
    } else if (strcmp(counter_name, "read_rsp_fail") == 0) {
        s_telemetry.read_rsp_fail++;
    } else if (strcmp(counter_name, "report_attr_ok") == 0) {
        s_telemetry.report_attr_ok++;
    } else if (strcmp(counter_name, "report_attr_unchanged") == 0) {
        s_telemetry.report_attr_unchanged++;
    } else if (strcmp(counter_name, "report_cfg_req") == 0) {
        s_telemetry.report_cfg_req++;
    } else if (strcmp(counter_name, "report_cfg_rsp_ok") == 0) {
        s_telemetry.report_cfg_rsp_ok++;
    } else if (strcmp(counter_name, "report_cfg_rsp_fail") == 0) {
        s_telemetry.report_cfg_rsp_fail++;
    } else if (strcmp(counter_name, "reinterview") == 0) {
        s_telemetry.reinterviews++;
    }
    device_table_unlock();
}

void device_table_get_telemetry(device_table_telemetry_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!device_table_lock()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_telemetry;
    device_table_unlock();
}

void device_table_get_network_summary(device_table_network_summary_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!device_table_lock()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied || !d->in_network) {
            continue;
        }
        out->nodes_total++;
        if (d->silence_level == 1) {
            out->nodes_silent_temp++;
        } else if (d->silence_level >= 2) {
            out->nodes_absent_prolonged++;
        }
        switch (d->norm_type) {
        case DEVICE_NORM_SWITCH:
            out->nodes_switch++;
            break;
        case DEVICE_NORM_TEMP_HUMIDITY:
            out->nodes_temp_humidity++;
            break;
        case DEVICE_NORM_PRESENCE:
            out->nodes_presence++;
            break;
        default:
            out->nodes_unknown++;
            break;
        }
    }
    device_table_unlock();
}

size_t device_table_copy_devices(device_record_t *out, size_t max_out)
{
    if (out == NULL || max_out == 0U) {
        return 0U;
    }
    if (!device_table_lock()) {
        return 0U;
    }
    size_t n = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES && n < max_out; ++i) {
        if (!s_devices[i].occupied) {
            continue;
        }
        out[n++] = s_devices[i];
    }
    device_table_unlock();
    return n;
}

bool device_table_copy_device_at(size_t slot, device_record_t *out)
{
    if (out == NULL || slot >= DEVICE_TABLE_MAX_DEVICES) {
        return false;
    }
    if (!device_table_lock()) {
        return false;
    }
    if (!s_devices[slot].occupied) {
        device_table_unlock();
        return false;
    }
    *out = s_devices[slot];
    device_table_unlock();
    return true;
}

size_t device_table_get_known_short_addrs(uint16_t *out, size_t max_out)
{
    if (out == NULL || max_out == 0) {
        return 0;
    }
    if (!device_table_lock()) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES && n < max_out; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied || !d->in_network) {
            continue;
        }
        if (d->short_addr == 0x0000U || d->short_addr == 0xFFFFU) {
            continue;
        }
        out[n++] = d->short_addr;
    }
    device_table_unlock();
    return n;
}

bool device_table_has_short_addr(uint16_t short_addr)
{
    if (!device_table_lock()) {
        return false;
    }
    const bool found = (find_by_short(short_addr) != NULL);
    device_table_unlock();
    return found;
}

static device_endpoint_record_t *find_endpoint_for_reading(uint16_t short_addr, uint8_t ep_id)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        return NULL;
    }
    return get_or_create_endpoint(rec, ep_id);
}

static void endpoint_readings_touch(device_record_t *rec, device_endpoint_record_t *ep)
{
    if (rec == NULL || ep == NULL) {
        return;
    }
    const double now_s = timebase_now_s();
    ep->last_readings_update_s = now_s;
    rec->last_seen_s = now_s;
}

bool device_table_note_reading_temperature(uint16_t short_addr, uint8_t ep, int16_t value_0_01_c)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (value_0_01_c == (int16_t)0x8000) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_temperature && ep_rec->temperature_0_01_c == value_0_01_c) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_temperature = true;
    ep_rec->temperature_0_01_c = value_0_01_c;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_humidity(uint16_t short_addr, uint8_t ep, uint16_t value_0_01_pct)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (value_0_01_pct == 0xFFFFU) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_humidity && ep_rec->humidity_0_01_pct == value_0_01_pct) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_humidity = true;
    ep_rec->humidity_0_01_pct = value_0_01_pct;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_on_off(uint16_t short_addr, uint8_t ep, bool on)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_on_off && ep_rec->on_off == on) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_on_off = true;
    ep_rec->on_off = on;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_occupancy(uint16_t short_addr, uint8_t ep, uint8_t occupancy_bitmap)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_occupancy && ep_rec->occupancy_bitmap == occupancy_bitmap) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_occupancy = true;
    ep_rec->occupancy_bitmap = occupancy_bitmap;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_illuminance(uint16_t short_addr, uint8_t ep, uint16_t measured_value_raw)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (measured_value_raw == 0xFFFFU) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_illuminance && ep_rec->illuminance_measured_value == measured_value_raw) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_illuminance = true;
    ep_rec->illuminance_measured_value = measured_value_raw;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_pressure(uint16_t short_addr, uint8_t ep, int16_t measured_value_0_1_kpa)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (measured_value_0_1_kpa == (int16_t)0x8000) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_pressure && ep_rec->pressure_0_1_kpa == measured_value_0_1_kpa) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_pressure = true;
    ep_rec->pressure_0_1_kpa = measured_value_0_1_kpa;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_ias_zone_status(uint16_t short_addr, uint8_t ep, uint16_t zone_status)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (ep_rec->has_ias_zone_status && ep_rec->ias_zone_status == zone_status) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_ias_zone_status = true;
    ep_rec->ias_zone_status = zone_status;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_battery_voltage(uint16_t short_addr, uint8_t ep, uint8_t voltage_100mv_units)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (voltage_100mv_units == 0xFFU) {
        device_table_unlock();
        return false;
    }
    const uint16_t mv = (uint16_t)voltage_100mv_units * 100U;
    if (ep_rec->has_power_battery_voltage && ep_rec->battery_mv == mv) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_power_battery_voltage = true;
    ep_rec->battery_mv = mv;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

bool device_table_note_reading_battery_pct(uint16_t short_addr, uint8_t ep, uint8_t percentage_remaining_half_pct)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    device_endpoint_record_t *ep_rec = find_endpoint_for_reading(short_addr, ep);
    if (rec == NULL || ep_rec == NULL) {
        device_table_unlock();
        return false;
    }
    if (percentage_remaining_half_pct == 0xFFU) {
        device_table_unlock();
        return false;
    }
    uint16_t p = (uint16_t)percentage_remaining_half_pct / 2U;
    if (p > 100U) {
        p = 100U;
    }
    const uint8_t pct = (uint8_t)p;
    if (ep_rec->has_power_battery_pct && ep_rec->battery_pct == pct) {
        device_table_unlock();
        return false;
    }
    ep_rec->has_power_battery_pct = true;
    ep_rec->battery_pct = pct;
    endpoint_readings_touch(rec, ep_rec);
    device_table_unlock();
    return true;
}

static bool endpoint_emit_poll_reads(device_table_zcl_read_req_fn_t fn, uint16_t short_addr, device_endpoint_record_t *ep)
{
    bool emitted = false;
    if (endpoint_has_cluster(ep, 0x0402U)) {
        fn(short_addr, ep->endpoint_id, 0x0402U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0405U)) {
        fn(short_addr, ep->endpoint_id, 0x0405U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0406U)) {
        fn(short_addr, ep->endpoint_id, 0x0406U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0006U)) {
        fn(short_addr, ep->endpoint_id, 0x0006U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0400U)) {
        fn(short_addr, ep->endpoint_id, 0x0400U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0403U)) {
        fn(short_addr, ep->endpoint_id, 0x0403U, 0x0000U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0001U)) {
        fn(short_addr, ep->endpoint_id, 0x0001U, 0x0020U);
        fn(short_addr, ep->endpoint_id, 0x0001U, 0x0021U);
        emitted = true;
    }
    if (endpoint_has_cluster(ep, 0x0500U)) {
        fn(short_addr, ep->endpoint_id, 0x0500U, 0x0002U);
        emitted = true;
    }
    return emitted;
}

bool device_table_get_health_probe(uint16_t short_addr, uint8_t *ep_out, uint16_t *cluster_id_out, uint16_t *attr_id_out)
{
    if (!device_table_lock()) {
        return false;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL || ep_out == NULL || cluster_id_out == NULL || attr_id_out == NULL) {
        device_table_unlock();
        return false;
    }

    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t *ep = &rec->endpoints[i];
        if (!ep->used) {
            continue;
        }
        if (endpoint_has_cluster(ep, 0x0000U)) {
            *ep_out = ep->endpoint_id;
            *cluster_id_out = 0x0000U;
            *attr_id_out = 0x0000U;
            device_table_unlock();
            return true;
        }
        if (endpoint_has_cluster(ep, 0x0402U)) {
            *ep_out = ep->endpoint_id;
            *cluster_id_out = 0x0402U;
            *attr_id_out = 0x0000U;
            device_table_unlock();
            return true;
        }
        if (endpoint_has_cluster(ep, 0x0006U)) {
            *ep_out = ep->endpoint_id;
            *cluster_id_out = 0x0006U;
            *attr_id_out = 0x0000U;
            device_table_unlock();
            return true;
        }
    }
    device_table_unlock();
    return false;
}

void device_table_note_poll_request(uint16_t short_addr, uint8_t ep)
{
    if (!device_table_lock()) {
        return;
    }
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        device_table_unlock();
        return;
    }
    device_endpoint_record_t *ep_rec = find_endpoint(rec, ep);
    if (ep_rec == NULL) {
        device_table_unlock();
        return;
    }
    ep_rec->last_poll_read_s = timebase_now_s();
    device_table_unlock();
}

void device_table_request_sensor_poll_reads(device_table_zcl_read_req_fn_t fn)
{
    if (fn == NULL) {
        return;
    }
    if (!device_table_lock()) {
        return;
    }
    const double now_s = timebase_now_s();
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        device_record_t *rec = &s_devices[i];
        if (!rec->occupied || !rec->in_network || rec->short_addr == 0x0000U || rec->short_addr == 0xFFFFU) {
            continue;
        }
        for (size_t e = 0; e < DEVICE_TABLE_MAX_ENDPOINTS; ++e) {
            device_endpoint_record_t *ep = &rec->endpoints[e];
            if (!ep->used) {
                continue;
            }
            if (ep->last_readings_update_s > 0.0 && (now_s - ep->last_readings_update_s) < SENSOR_POLL_FRESH_S) {
                continue;
            }
            if (endpoint_emit_poll_reads(fn, rec->short_addr, ep)) {
                ep->last_poll_read_s = now_s;
            }
        }
    }
    device_table_unlock();
}

void device_table_persist_cache(void)
{
    device_table_cache_head_t head = {0};
    double now_s = 0.0;
    double dirty_age_s = 0.0;
    size_t occ = 0;
    uint32_t next_write_idx = 0;
    uint32_t snapshot_gen = 0;

    if (!device_table_lock()) {
        return;
    }
    (void)canonicalize_device_records();
    if (!s_cache_dirty) {
        device_table_unlock();
        return;
    }
    now_s = timebase_now_s();
    dirty_age_s = (s_cache_last_dirty_s > 0.0) ? (now_s - s_cache_last_dirty_s) : 0.0;
    if (s_cache_last_dirty_s > 0.0 && dirty_age_s < CACHE_FLUSH_DEBOUNCE_S) {
        device_table_unlock();
        return;
    }
    occ = count_occupied_devices();
    next_write_idx = s_cache_flash_write_count + 1U;
    snapshot_gen = s_cache_dirty_generation;
    head.version = CACHE_VER;
    head.device_count = (uint32_t)occ;
    head.device_slots = DEVICE_TABLE_MAX_DEVICES;
    head.record_size = sizeof(device_cache_record_t);
    head.chunk_slots = CACHE_CHUNK_SLOTS;
    head.chunk_size = sizeof(s_chunk_scratch);
    device_table_unlock();

    ESP_LOGW(TAG,
             "[T+%07.3f] FLASH_WRITE_BEGIN zb_cache seq=%" PRIu32 " occupied=%u dirty_age=%.3f s bytes=%u",
             now_s, next_write_idx, (unsigned)occ, dirty_age_s, (unsigned)cache_payload_reserved_bytes());

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CACHE_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS nvs_open RW '%s' fallo al guardar cache: %s", timebase_now_s(), CACHE_NS, esp_err_to_name(err));
        return;
    }

    for (size_t c = 0; c < CACHE_NUM_CHUNKS; ++c) {
        char key[16];
        if (!device_table_lock()) {
            nvs_close(nvs);
            return;
        }
        memset(s_chunk_scratch, 0, sizeof(s_chunk_scratch));
        for (size_t i = 0; i < CACHE_CHUNK_SLOTS; ++i) {
            const size_t src_idx = (c * CACHE_CHUNK_SLOTS) + i;
            cache_record_from_device(&s_chunk_scratch[i], &s_devices[src_idx]);
        }
        device_table_unlock();
        cache_chunk_key(key, sizeof(key), (unsigned)c);
        const size_t chunk_bytes = sizeof(s_chunk_scratch);
        err = nvs_set_blob(nvs, key, &s_chunk_scratch[0], chunk_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[T+%07.3f] NVS guardar '%s' (%u B) fallo: %s", timebase_now_s(), key, (unsigned)chunk_bytes, esp_err_to_name(err));
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                ESP_LOGE(TAG, "Sugerencia: reservar al menos 0x20000 para 'nvs' y mantener cache fija a ocupacion maxima");
            }
            nvs_close(nvs);
            return;
        }
    }

    {
        esp_err_t er = nvs_erase_key(nvs, CACHE_KEY_LEGACY);
        if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS borrar clave legacy '%s': %s", CACHE_KEY_LEGACY, esp_err_to_name(er));
        }
    }

    err = nvs_set_blob(nvs, "dt2_head", &head, sizeof(head));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] NVS guardar dt2_head (%u B) fallo: %s", timebase_now_s(), (unsigned)sizeof(head), esp_err_to_name(err));
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGE(TAG, "Sugerencia: ampliar particion 'nvs' en partitions.csv; ver README");
        }
        nvs_close(nvs);
        return;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] NVS commit cache fallo: %s", timebase_now_s(), esp_err_to_name(err));
        return;
    }

    if (!device_table_lock()) {
        return;
    }
    s_cache_loaded = true;
    s_cache_flash_write_count = next_write_idx;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "chunked-fixed");
    if (s_cache_dirty && s_cache_dirty_generation == snapshot_gen) {
        s_cache_dirty = false;
        s_cache_last_dirty_s = 0.0;
    }
    device_table_unlock();

    ESP_LOGW(TAG,
             "[T+%07.3f] FLASH_WRITE_END zb_cache seq=%" PRIu32 " commit=OK formato=%s head=%u B + %u x %u B reservado_max=%u B ocupados=%u",
             timebase_now_s(), next_write_idx, "chunked-fixed", (unsigned)sizeof(head), (unsigned)CACHE_NUM_CHUNKS,
             (unsigned)sizeof(s_chunk_scratch), (unsigned)cache_payload_reserved_bytes(), (unsigned)occ);
    if (device_table_lock()) {
        log_devices_table_contents("NVS escrito");
        device_table_unlock();
    }
}

void device_table_dump_json(void)
{
    if (!device_table_lock()) {
        return;
    }
    (void)canonicalize_device_records();
    size_t silent_count = 0;
    device_table_network_summary_t summary = {0};
    device_table_route_summary_t routes[DEVICE_TABLE_MAX_ROUTE_SUMMARY] = {0};
    size_t routes_len = 0;
    device_table_get_network_summary(&summary);

    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].silent) {
            silent_count++;
        }
        if (!s_devices[i].occupied || !s_devices[i].in_network || s_devices[i].parent_short == 0x0000 || s_devices[i].parent_short == 0xFFFF) {
            continue;
        }
        bool found = false;
        for (size_t r = 0; r < routes_len; ++r) {
            if (routes[r].parent_short == s_devices[i].parent_short) {
                routes[r].children++;
                found = true;
                break;
            }
        }
        if (!found && routes_len < DEVICE_TABLE_MAX_ROUTE_SUMMARY) {
            routes[routes_len].parent_short = s_devices[i].parent_short;
            routes[routes_len].children = 1;
            routes_len++;
        }
    }
    s_telemetry.silent_nodes = (uint32_t)silent_count;

    printf("{\n");
    printf("  \"ts_s\": %.3f,\n", timebase_now_s());
    printf("  \"cache_blob\": {\n");
    printf("    \"version\": %u,\n", (unsigned)CACHE_VER);
    printf("    \"loaded\": %s,\n", s_cache_loaded ? "true" : "false");
    printf("    \"nvs_format\": \"%s\",\n", s_cache_nvs_format);
    printf("    \"persist_minimal\": true,\n");
    printf("    \"bytes_head\": %u,\n", (unsigned)sizeof(device_table_cache_head_t));
    printf("    \"bytes_per_record\": %u,\n", (unsigned)sizeof(device_cache_record_t));
    printf("    \"bytes_per_chunk\": %u,\n", (unsigned)sizeof(s_chunk_scratch));
    printf("    \"bytes_reserved_max\": %u,\n", (unsigned)cache_payload_reserved_bytes());
    printf("    \"flash_write_count\": %" PRIu32 ",\n", s_cache_flash_write_count);
    printf("    \"device_slots\": %u\n", (unsigned)DEVICE_TABLE_MAX_DEVICES);
    printf("  },\n");
    printf("  \"telemetry\": {\n");
    printf("    \"interview_started\": %" PRIu32 ",\n", s_telemetry.interview_started);
    printf("    \"interview_completed\": %" PRIu32 ",\n", s_telemetry.interview_completed);
    printf("    \"interview_retries\": %" PRIu32 ",\n", s_telemetry.interview_retries);
    printf("    \"interview_failed\": %" PRIu32 ",\n", s_telemetry.interview_failed);
    printf("    \"reinterviews\": %" PRIu32 ",\n", s_telemetry.reinterviews);
    printf("    \"read_req\": %" PRIu32 ",\n", s_telemetry.read_req);
    printf("    \"read_rsp_ok\": %" PRIu32 ",\n", s_telemetry.read_rsp_ok);
    printf("    \"read_rsp_fail\": %" PRIu32 ",\n", s_telemetry.read_rsp_fail);
    printf("    \"report_attr_ok\": %" PRIu32 ",\n", s_telemetry.report_attr_ok);
    printf("    \"report_attr_unchanged\": %" PRIu32 ",\n", s_telemetry.report_attr_unchanged);
    printf("    \"report_cfg_req\": %" PRIu32 ",\n", s_telemetry.report_cfg_req);
    printf("    \"report_cfg_rsp_ok\": %" PRIu32 ",\n", s_telemetry.report_cfg_rsp_ok);
    printf("    \"report_cfg_rsp_fail\": %" PRIu32 ",\n", s_telemetry.report_cfg_rsp_fail);
    printf("    \"device_announce\": %" PRIu32 ",\n", s_telemetry.device_announce);
    printf("    \"device_update\": %" PRIu32 ",\n", s_telemetry.device_update);
    printf("    \"device_rejoin\": %" PRIu32 ",\n", s_telemetry.device_rejoin);
    printf("    \"silent_nodes\": %" PRIu32 ",\n", s_telemetry.silent_nodes);
    printf("    \"zdo_latency_avg_ms\": %.2f,\n", s_telemetry.zdo_latency_avg_ms);
    printf("    \"zcl_latency_avg_ms\": %.2f\n", s_telemetry.zcl_latency_avg_ms);
    printf("  },\n");
    printf("  \"network_summary\": {\n");
    printf("    \"nodes_total\": %" PRIu32 ",\n", summary.nodes_total);
    printf("    \"nodes_silent_temp\": %" PRIu32 ",\n", summary.nodes_silent_temp);
    printf("    \"nodes_absent_prolonged\": %" PRIu32 ",\n", summary.nodes_absent_prolonged);
    printf("    \"nodes_switch\": %" PRIu32 ",\n", summary.nodes_switch);
    printf("    \"nodes_temp_humidity\": %" PRIu32 ",\n", summary.nodes_temp_humidity);
    printf("    \"nodes_presence\": %" PRIu32 ",\n", summary.nodes_presence);
    printf("    \"nodes_unknown\": %" PRIu32 ",\n", summary.nodes_unknown);
    printf("    \"routes\": [");
    for (size_t r = 0; r < routes_len; ++r) {
        printf("%s\n      {\"parent_short\": \"0x%04X\", \"children\": %" PRIu32 "}", (r == 0) ? "" : ",", routes[r].parent_short,
               routes[r].children);
    }
    if (routes_len > 0) {
        printf("\n    ");
    }
    printf("]\n");
    printf("  },\n");
    printf("  \"devices\": [\n");
    {
        bool first_device = true;
        for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
            const device_record_t *d = &s_devices[i];
            if (!d->occupied) {
                continue;
            }
            printf("%s", first_device ? "" : ",\n");
            first_device = false;
            printf("    {\n");
            printf("      \"ieee\": \"0x%016" PRIX64 "\",\n", d->ieee);
            printf("      \"short\": \"0x%04X\",\n", d->short_addr);
            printf("      \"in_network\": %s,\n", d->in_network ? "true" : "false");
            printf("      \"seen_in_device_annce\": %s,\n", d->seen_in_device_annce ? "true" : "false");
            printf("      \"authorized\": %s,\n", d->authorized ? "true" : "false");
            printf("      \"authorization_type\": %u,\n", (unsigned)d->authorization_type);
            printf("      \"authorization_status\": %u,\n", (unsigned)d->authorization_status);
            printf("      \"last_seen_s\": %.3f,\n", d->last_seen_s);
            printf("      \"lqi\": %u,\n", d->lqi);
            printf("      \"rssi\": %d,\n", d->rssi);
            printf("      \"manufacturer\": \"%s\",\n", d->manufacturer);
            printf("      \"model\": \"%s\",\n", d->model);
            printf("      \"parent_short\": \"0x%04X\",\n", d->parent_short);
            printf("      \"update_status\": %u,\n", (unsigned)d->update_status);
            printf("      \"tc_action\": %u,\n", (unsigned)d->tc_action);
            printf("      \"norm_type\": \"%s\",\n", d->norm_name);
            printf("      \"silent\": %s,\n", d->silent ? "true" : "false");
            printf("      \"silence_level\": %u,\n", (unsigned)d->silence_level);
            printf("      \"interview_phase\": %u,\n", (unsigned)d->interview_phase);
            printf("      \"interview_retries\": %u,\n", (unsigned)d->interview_retries);
            printf("      \"report_cfg_ok\": %s,\n", d->report_cfg_ok ? "true" : "false");
            printf("      \"report_cfg_retries\": %u,\n", (unsigned)d->report_cfg_retries);
            printf("      \"last_interview_s\": %.3f,\n", d->last_interview_s);
            printf("      \"last_report_cfg_s\": %.3f,\n", d->last_report_cfg_s);
            printf("      \"node_desc\": {\n");
            printf("        \"node_desc_flags\": \"0x%04X\",\n", d->node_desc_flags);
            printf("        \"mac_capability_flags\": \"0x%02X\",\n", d->mac_capability_flags);
            printf("        \"manufacturer_code\": \"0x%04X\",\n", d->manufacturer_code);
            printf("        \"max_buf_size\": %u,\n", (unsigned)d->max_buf_size);
            printf("        \"max_incoming_transfer_size\": %u,\n", (unsigned)d->max_incoming_transfer_size);
            printf("        \"server_mask\": \"0x%04X\",\n", d->server_mask);
            printf("        \"max_outgoing_transfer_size\": %u,\n", (unsigned)d->max_outgoing_transfer_size);
            printf("        \"desc_capability_field\": \"0x%02X\"\n", d->desc_capability_field);
            printf("      },\n");
            printf("      \"endpoints\": [");
            {
                bool first_ep = true;
                for (size_t e = 0; e < DEVICE_TABLE_MAX_ENDPOINTS; ++e) {
                    const device_endpoint_record_t *ep = &d->endpoints[e];
                    if (!ep->used) {
                        continue;
                    }
                    printf("%s\n        {", first_ep ? "" : ",");
                    first_ep = false;
                    printf("\n          \"endpoint_id\": %u,\n", ep->endpoint_id);
                    printf("          \"profile_id\": \"0x%04X\",\n", ep->profile_id);
                    printf("          \"device_id\": \"0x%04X\",\n", ep->device_id);
                    printf("          \"device_version\": %u,\n", ep->device_version);
                    printf("          \"input_clusters\": [");
                    for (size_t c = 0; c < ep->input_clusters_len; ++c) {
                        printf("%s\"0x%04X\"", (c == 0) ? "" : ",", ep->input_clusters[c]);
                    }
                    printf("],\n");
                    printf("          \"output_clusters\": [");
                    for (size_t c = 0; c < ep->output_clusters_len; ++c) {
                        printf("%s\"0x%04X\"", (c == 0) ? "" : ",", ep->output_clusters[c]);
                    }
                    printf("],\n");
                    printf("          \"last_poll_read_s\": %.3f,\n", ep->last_poll_read_s);
                    printf("          \"last_readings_update_s\": %.3f", ep->last_readings_update_s);
                    if (ep->has_temperature) {
                        printf(",\n          \"temperature_c\": %.2f", (double)ep->temperature_0_01_c / 100.0);
                    }
                    if (ep->has_humidity) {
                        printf(",\n          \"humidity_pct\": %.2f", (double)ep->humidity_0_01_pct / 100.0);
                    }
                    if (ep->has_on_off) {
                        printf(",\n          \"on_off\": %s", ep->on_off ? "true" : "false");
                    }
                    if (ep->has_occupancy) {
                        printf(",\n          \"occupancy_bitmap\": \"0x%02X\"", (unsigned)ep->occupancy_bitmap);
                    }
                    if (ep->has_illuminance) {
                        printf(",\n          \"illuminance_measured_value\": %u", (unsigned)ep->illuminance_measured_value);
                        if (ep->illuminance_measured_value != 0U) {
                            const double lx = pow(10.0, ((double)ep->illuminance_measured_value - 1.0) / 10000.0);
                            printf(",\n          \"illuminance_lux\": %.6g", lx);
                        }
                    }
                    if (ep->has_pressure) {
                        printf(",\n          \"pressure_kpa\": %.2f", (double)ep->pressure_0_1_kpa / 10.0);
                    }
                    if (ep->has_ias_zone_status) {
                        printf(",\n          \"ias_zone_status\": \"0x%04X\"", (unsigned)ep->ias_zone_status);
                    }
                    if (ep->has_power_battery_voltage) {
                        printf(",\n          \"power_battery_mv\": %u", (unsigned)ep->battery_mv);
                    }
                    if (ep->has_power_battery_pct) {
                        printf(",\n          \"power_battery_pct\": %u", (unsigned)ep->battery_pct);
                    }
                    printf("\n        }");
                }
                if (!first_ep) {
                    printf("\n      ");
                }
            }
            printf("]\n");
            printf("    }");
        }
    }
    printf("\n  ]\n");
    printf("}\n");
    device_table_unlock();
}
