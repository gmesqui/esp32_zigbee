#include "device_table.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "timebase.h"
#include "zb_coordinator.h"

static const char *TAG = "device_table";
static device_record_t s_devices[DEVICE_TABLE_MAX_DEVICES];
static device_table_telemetry_t s_telemetry;
static const char *CACHE_NS = "zb_cache";
/** Blob monolítico legado (puede fallar por tamaño/fragmentación en NVS pequeño). */
static const char *CACHE_KEY_LEGACY = "devtab_v2";
#define DEVICE_TABLE_MAX_ROUTE_SUMMARY 32
#define CACHE_VER 2U
/** Dispositivos por clave NVS (blobs más pequeños = menos fallos en partición ~24 KiB). */
#define CACHE_CHUNK_SLOTS 8
#define CACHE_NUM_CHUNKS (DEVICE_TABLE_MAX_DEVICES / CACHE_CHUNK_SLOTS)

_Static_assert(DEVICE_TABLE_MAX_DEVICES % CACHE_CHUNK_SLOTS == 0, "DEVICE_TABLE_MAX_DEVICES debe ser múltiplo de CACHE_CHUNK_SLOTS");
static bool s_cache_loaded = false;
static char s_cache_nvs_format[12] = "none";
/** True si la tabla/telemetria persistible cambio; evita escrituras NVS periodicas. */
static bool s_cache_dirty = false;

static void mark_cache_dirty(void)
{
    s_cache_dirty = true;
}

typedef struct {
    uint32_t version;
    device_table_telemetry_t telemetry;
    device_record_t devices[DEVICE_TABLE_MAX_DEVICES];
} device_table_cache_blob_t;
static device_table_cache_blob_t s_cache_blob;

typedef struct {
    uint32_t version;
    device_table_telemetry_t telemetry;
} device_table_cache_head_t;

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

static void sync_s_cache_blob_mirror(void)
{
    memset(&s_cache_blob, 0, sizeof(s_cache_blob));
    s_cache_blob.version = CACHE_VER;
    s_cache_blob.telemetry = s_telemetry;
    memcpy(s_cache_blob.devices, s_devices, sizeof(s_devices));
}

static void log_telemetry_summary(const char *prefix)
{
    ESP_LOGI(TAG,
             "%s telemetria: interview_started=%" PRIu32 " completed=%" PRIu32 " failed=%" PRIu32 " reinterviews=%" PRIu32
             " read_rsp_ok=%" PRIu32 " device_ann=%" PRIu32 " device_upd=%" PRIu32,
             prefix, s_telemetry.interview_started, s_telemetry.interview_completed, s_telemetry.interview_failed, s_telemetry.reinterviews,
             s_telemetry.read_rsp_ok, s_telemetry.device_announce, s_telemetry.device_update);
}

static void log_devices_table_contents(const char *prefix)
{
    const size_t occ = count_occupied_devices();
    ESP_LOGI(TAG, "%s tabla RAM: ocupados=%u/%u sizeof(device_record_t)=%u sizeof(blob_legacy)=%u", prefix, (unsigned)occ,
             (unsigned)DEVICE_TABLE_MAX_DEVICES, (unsigned)sizeof(device_record_t), (unsigned)sizeof(device_table_cache_blob_t));
    unsigned logged = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES && logged < 16U; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
            continue;
        }
        ESP_LOGI(TAG, "%s  [%u] ieee=0x%016" PRIX64 " short=0x%04X mfg='%s' model='%s' last_seen=%.3f", prefix, (unsigned)i, d->ieee,
                 d->short_addr, d->manufacturer, d->model, d->last_seen_s);
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

    for (unsigned c = 0; c < CACHE_NUM_CHUNKS; ++c) {
        char key[16];
        cache_chunk_key(key, sizeof(key), c);
        len = CACHE_CHUNK_SLOTS * sizeof(device_record_t);
        err = nvs_get_blob(nvs, key, &s_devices[c * CACHE_CHUNK_SLOTS], &len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS clave '%s' fallo: %s", key, esp_err_to_name(err));
            return false;
        }
        if (len != CACHE_CHUNK_SLOTS * sizeof(device_record_t)) {
            ESP_LOGW(TAG, "NVS '%s' tamano leido=%u esperado=%u", key, (unsigned)len,
                     (unsigned)(CACHE_CHUNK_SLOTS * sizeof(device_record_t)));
            return false;
        }
    }

    s_telemetry = head.telemetry;
    s_cache_loaded = true;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "chunked");
    sync_s_cache_blob_mirror();
    ESP_LOGI(TAG, "[T+%07.3f] NVS cache cargada formato=chunked head=%u B x%u chunks de %u B c/u", timebase_now_s(),
             (unsigned)sizeof(device_table_cache_head_t), (unsigned)CACHE_NUM_CHUNKS,
             (unsigned)(CACHE_CHUNK_SLOTS * sizeof(device_record_t)));
    log_telemetry_summary("NVS leido");
    log_devices_table_contents("NVS leido");
    return true;
}

static bool load_cache_legacy(nvs_handle_t nvs)
{
    memset(&s_cache_blob, 0, sizeof(s_cache_blob));
    size_t len = sizeof(s_cache_blob);
    esp_err_t err = nvs_get_blob(nvs, CACHE_KEY_LEGACY, &s_cache_blob, &len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS '%s' (legacy): %s", CACHE_KEY_LEGACY, esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(s_cache_blob)) {
        ESP_LOGW(TAG, "NVS '%s' tamano leido=%u esperado=%u (layout cambio o corrupto)", CACHE_KEY_LEGACY, (unsigned)len,
                 (unsigned)sizeof(s_cache_blob));
        return false;
    }
    if (s_cache_blob.version != CACHE_VER) {
        ESP_LOGW(TAG, "NVS '%s' version=%" PRIu32 " invalida", CACHE_KEY_LEGACY, s_cache_blob.version);
        return false;
    }
    memcpy(s_devices, s_cache_blob.devices, sizeof(s_devices));
    s_telemetry = s_cache_blob.telemetry;
    s_cache_loaded = true;
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "legacy");
    ESP_LOGI(TAG, "[T+%07.3f] NVS cache cargada formato=legacy clave=%s blob=%u B", timebase_now_s(), CACHE_KEY_LEGACY,
             (unsigned)sizeof(s_cache_blob));
    log_telemetry_summary("NVS leido");
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
    rec->ieee = ieee;
    return rec;
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
    bool onoff = false, temp = false, hum = false, occ = false;
    for (size_t i = 0; i < rec->clusters_in_len; ++i) {
        const uint16_t c = rec->clusters_in[i];
        onoff |= (c == 0x0006);
        temp |= (c == 0x0402);
        hum |= (c == 0x0405);
        occ |= (c == 0x0406 || c == 0x0500);
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

static bool push_unique_u8(uint8_t *arr, size_t *len, size_t max, uint8_t v)
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

void device_table_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    memset(&s_cache_blob, 0, sizeof(s_cache_blob));
    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "none");

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CACHE_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS namespace '%s' inexistente (primer arranque o sin guardados previos)", timebase_now_s(), CACHE_NS);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] NVS nvs_open RO '%s' fallo: %s", timebase_now_s(), CACHE_NS, esp_err_to_name(err));
        return;
    }

    if (!load_cache_chunked(nvs) && !load_cache_legacy(nvs)) {
        ESP_LOGI(TAG, "[T+%07.3f] NVS sin cache operacional valida (ni chunked ni legacy)", timebase_now_s());
    }
    nvs_close(nvs);
    s_cache_dirty = false;
}

void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi)
{
    device_record_t *rec = ensure_device(ieee);
    rec->short_addr = short_addr;
    rec->rssi = rssi;
    rec->lqi = lqi;
    rec->last_seen_s = timebase_now_s();
    mark_cache_dirty();
}

void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model)
{
    device_record_t *rec = ensure_device(ieee);
    rec->short_addr = short_addr;
    rec->device_id = device_id;
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL) {
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL) {
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
    update_norm_type(rec);
    mark_cache_dirty();
}

void device_table_update_from_trace(const zb_trace_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    zb_coordinator_note_inbound_device_traffic(meta->src_short);
    /* No crear entradas con IEEE sintetico: solo actualizar dispositivos ya descubiertos por ZDO. */
    device_record_t *rec = find_by_short(meta->src_short);
    if (rec == NULL) {
        return;
    }
    rec->rssi = meta->rssi;
    rec->lqi = meta->lqi;
    rec->last_seen_s = timebase_now_s();
}

void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL && manufacturer[0] != '\0') {
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL && model[0] != '\0') {
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
    update_norm_type(rec);
    mark_cache_dirty();
}

void device_table_update_node_desc(uint16_t short_addr, uint16_t manufacturer_code, uint8_t mac_capability_flags)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }
    rec->last_seen_s = timebase_now_s();
    rec->state_flags = ((uint32_t)manufacturer_code << 8) | (uint32_t)mac_capability_flags;
    mark_cache_dirty();
}

void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t device_id, const uint16_t *clusters_in,
                                     size_t clusters_in_len, const uint16_t *clusters_out, size_t clusters_out_len)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }

    rec->device_id = device_id;
    rec->last_seen_s = timebase_now_s();
    (void)push_unique_u8(rec->endpoints, &rec->endpoints_len, DEVICE_TABLE_MAX_ENDPOINTS, endpoint);

    if (clusters_in != NULL) {
        for (size_t i = 0; i < clusters_in_len; ++i) {
            (void)push_unique_u16(rec->clusters_in, &rec->clusters_in_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_in[i]);
        }
    }
    if (clusters_out != NULL) {
        for (size_t i = 0; i < clusters_out_len; ++i) {
            (void)push_unique_u16(rec->clusters_out, &rec->clusters_out_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_out[i]);
        }
    }
    update_norm_type(rec);
    mark_cache_dirty();
}

void device_table_update_device_update(uint64_t ieee, uint16_t short_addr, uint16_t parent_short, uint8_t status)
{
    device_record_t *rec = ensure_device(ieee);
    rec->short_addr = short_addr;
    rec->parent_short = parent_short;
    rec->update_status = status;
    rec->last_seen_s = timebase_now_s();
    mark_cache_dirty();
}

void device_table_mark_interview(uint16_t short_addr, uint8_t phase, bool success, bool is_retry)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
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
    mark_cache_dirty();
}

void device_table_mark_report_cfg(uint16_t short_addr, bool success, bool is_retry)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        return;
    }
    rec->last_report_cfg_s = timebase_now_s();
    if (is_retry) {
        rec->report_cfg_retries++;
    }
    rec->report_cfg_ok = success;
    mark_cache_dirty();
}

void device_table_mark_silent(uint16_t short_addr, bool silent)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        return;
    }
    rec->silent = silent;
    if (!silent) {
        rec->silence_level = 0;
    } else if (rec->silence_level == 0) {
        rec->silence_level = 1;
    }
}

void device_table_mark_absent_prolonged(uint16_t short_addr, bool absent)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        return;
    }
    if (absent) {
        rec->silent = true;
        rec->silence_level = 2;
    } else if (rec->silence_level == 2) {
        rec->silence_level = 1;
    }
}

void device_table_note_latency(bool is_zdo, double latency_ms)
{
    if (latency_ms < 0.0) {
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
}

void device_table_inc_counter(const char *counter_name)
{
    if (counter_name == NULL) {
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
    } else if (strcmp(counter_name, "report_cfg_req") == 0) {
        s_telemetry.report_cfg_req++;
    } else if (strcmp(counter_name, "report_cfg_rsp_ok") == 0) {
        s_telemetry.report_cfg_rsp_ok++;
    } else if (strcmp(counter_name, "report_cfg_rsp_fail") == 0) {
        s_telemetry.report_cfg_rsp_fail++;
    } else if (strcmp(counter_name, "reinterview") == 0) {
        s_telemetry.reinterviews++;
    }
}

void device_table_get_telemetry(device_table_telemetry_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_telemetry;
}

void device_table_get_network_summary(device_table_network_summary_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
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
}

size_t device_table_get_known_short_addrs(uint16_t *out, size_t max_out)
{
    if (out == NULL || max_out == 0) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES && n < max_out; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
            continue;
        }
        if (d->short_addr == 0x0000 || d->short_addr == 0xFFFF) {
            continue;
        }
        out[n++] = d->short_addr;
    }
    return n;
}

bool device_table_has_short_addr(uint16_t short_addr)
{
    return (find_by_short(short_addr) != NULL);
}

void device_table_persist_cache(void)
{
    if (!s_cache_dirty) {
        return;
    }

    sync_s_cache_blob_mirror();
    const size_t occ = count_occupied_devices();

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CACHE_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS nvs_open RW '%s' fallo al guardar cache: %s", timebase_now_s(), CACHE_NS, esp_err_to_name(err));
        return;
    }

    for (unsigned c = 0; c < CACHE_NUM_CHUNKS; ++c) {
        char key[16];
        cache_chunk_key(key, sizeof(key), c);
        const size_t chunk_bytes = CACHE_CHUNK_SLOTS * sizeof(device_record_t);
        err = nvs_set_blob(nvs, key, &s_devices[c * CACHE_CHUNK_SLOTS], chunk_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[T+%07.3f] NVS guardar '%s' (%u B) fallo: %s", timebase_now_s(), key, (unsigned)chunk_bytes, esp_err_to_name(err));
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                ESP_LOGE(TAG, "Sugerencia: ampliar particion 'nvs' en partitions.csv (p. ej. 0xC000) y desplazar phy/zb_*; ver README");
            }
            nvs_close(nvs);
            return;
        }
    }

    /* Liberar espacio: blob monolítico legado ya no se usa. */
    esp_err_t er = nvs_erase_key(nvs, CACHE_KEY_LEGACY);
    if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS borrar clave legacy '%s': %s", CACHE_KEY_LEGACY, esp_err_to_name(er));
    }

    device_table_cache_head_t head = {
        .version = CACHE_VER,
        .telemetry = s_telemetry,
    };
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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] NVS commit cache fallo: %s", timebase_now_s(), esp_err_to_name(err));
        nvs_close(nvs);
        return;
    }

    snprintf(s_cache_nvs_format, sizeof(s_cache_nvs_format), "chunked");
    ESP_LOGI(TAG,
             "[T+%07.3f] NVS cache guardada en flash formato=chunked head=%u B + %u x %u B (dispositivos ocupados=%u) commit=OK",
             timebase_now_s(), (unsigned)sizeof(head), (unsigned)CACHE_NUM_CHUNKS, (unsigned)(CACHE_CHUNK_SLOTS * sizeof(device_record_t)),
             (unsigned)occ);
    log_telemetry_summary("NVS escrito");
    log_devices_table_contents("NVS escrito");
    s_cache_dirty = false;
    nvs_close(nvs);
}

void device_table_dump_json(void)
{
    size_t silent_count = 0;
    device_table_network_summary_t summary = {0};
    zb_coordinator_ram_snapshot_t coord = {0};
    device_table_route_summary_t routes[DEVICE_TABLE_MAX_ROUTE_SUMMARY] = {0};
    size_t routes_len = 0;
    device_table_get_network_summary(&summary);
    zb_coordinator_get_ram_snapshot(&coord);
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].silent) {
            silent_count++;
        }
        if (!s_devices[i].occupied || s_devices[i].parent_short == 0x0000 || s_devices[i].parent_short == 0xFFFF) {
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
    printf("    \"version\": %" PRIu32 ",\n", s_cache_blob.version);
    printf("    \"loaded\": %s,\n", s_cache_loaded ? "true" : "false");
    printf("    \"nvs_format\": \"%s\",\n", s_cache_nvs_format);
    printf("    \"bytes\": %u,\n", (unsigned)sizeof(s_cache_blob));
    printf("    \"device_slots\": %u\n", (unsigned)DEVICE_TABLE_MAX_DEVICES);
    printf("  },\n");
    printf("  \"coordinator_ram\": {\n");
    printf("    \"runtime\": {\n");
    printf("      \"has_network\": %s,\n", coord.runtime.has_network ? "true" : "false");
    printf("      \"channel\": %u,\n", coord.runtime.channel);
    printf("      \"pan_id\": \"0x%04X\",\n", coord.runtime.pan_id);
    printf("      \"short_addr\": \"0x%04X\"\n", coord.runtime.short_addr);
    printf("    },\n");
    printf("    \"interviews\": [");
    for (uint32_t i = 0; i < coord.interview_count; ++i) {
        const zb_coord_interview_dump_t *d = &coord.interviews[i];
        printf("%s{\"short\":\"0x%04X\",\"phase\":%u,\"retries\":%u,\"ep_count\":%u,\"ep_idx\":%u,\"node_desc_ok\":%s,\"active_ep_ok\":%s,"
               "\"simple_desc_ok\":%s,\"last_seen_s\":%.3f,\"last_interview_s\":%.3f}",
               (i == 0) ? "" : ",", d->short_addr, d->phase, d->retries, d->ep_count, d->ep_idx, d->node_desc_ok ? "true" : "false",
               d->active_ep_ok ? "true" : "false", d->simple_desc_ok ? "true" : "false", d->last_seen_s, d->last_interview_s);
    }
    printf("],\n");
    printf("    \"identity_jobs\": [");
    for (uint32_t i = 0; i < coord.identity_count; ++i) {
        const zb_coord_identity_dump_t *d = &coord.identities[i];
        printf("%s{\"short\":\"0x%04X\",\"ep\":%u,\"attempts\":%u,\"got_manufacturer\":%s,\"got_model\":%s}",
               (i == 0) ? "" : ",", d->short_addr, d->endpoint, d->attempts, d->got_manufacturer ? "true" : "false",
               d->got_model ? "true" : "false");
    }
    printf("]\n");
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
        printf("%s{\"parent_short\":\"0x%04X\",\"children\":%" PRIu32 "}", (r == 0) ? "" : ",", routes[r].parent_short, routes[r].children);
    }
    printf("]\n");
    printf("  },\n");
    printf("  \"devices\": [\n");
    bool first = true;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
            continue;
        }
        printf("%s", first ? "" : ",\n");
        first = false;
        printf("    {\n");
        printf("      \"ieee\": \"0x%016" PRIX64 "\",\n", d->ieee);
        printf("      \"short\": \"0x%04X\",\n", d->short_addr);
        printf("      \"device_id\": \"0x%04X\",\n", d->device_id);
        printf("      \"last_seen_s\": %.3f,\n", d->last_seen_s);
        printf("      \"lqi\": %u,\n", d->lqi);
        printf("      \"rssi\": %d,\n", d->rssi);
        printf("      \"manufacturer\": \"%s\",\n", d->manufacturer);
        printf("      \"model\": \"%s\",\n", d->model);
        printf("      \"state_flags\": %u,\n", (unsigned)d->state_flags);
        printf("      \"parent_short\": \"0x%04X\",\n", d->parent_short);
        printf("      \"norm_type\": \"%s\",\n", d->norm_name);
        printf("      \"silent\": %s,\n", d->silent ? "true" : "false");
        printf("      \"silence_level\": %u,\n", (unsigned)d->silence_level);
        printf("      \"interview_phase\": %u,\n", (unsigned)d->interview_phase);
        printf("      \"interview_retries\": %u,\n", (unsigned)d->interview_retries);
        printf("      \"report_cfg_ok\": %s,\n", d->report_cfg_ok ? "true" : "false");
        printf("      \"report_cfg_retries\": %u,\n", (unsigned)d->report_cfg_retries);
        printf("      \"last_interview_s\": %.3f,\n", d->last_interview_s);
        printf("      \"last_report_cfg_s\": %.3f,\n", d->last_report_cfg_s);
        printf("      \"last_poll_read_s\": %.3f,\n", d->last_poll_read_s);
        printf("      \"battery_mv\": %u,\n", d->battery_mv);
        printf("      \"battery_pct\": %u,\n", d->battery_pct);

        printf("      \"endpoints\": [");
        for (size_t e = 0; e < d->endpoints_len; ++e) {
            printf("%s%u", (e == 0) ? "" : ",", d->endpoints[e]);
        }
        printf("],\n");

        printf("      \"clusters_in\": [");
        for (size_t c = 0; c < d->clusters_in_len; ++c) {
            printf("%s\"0x%04X\"", (c == 0) ? "" : ",", d->clusters_in[c]);
        }
        printf("],\n");

        printf("      \"clusters_out\": [");
        for (size_t c = 0; c < d->clusters_out_len; ++c) {
            printf("%s\"0x%04X\"", (c == 0) ? "" : ",", d->clusters_out[c]);
        }
        printf("]\n");
        printf("    }");
    }
    printf("\n  ]\n");
    printf("}\n");
    ESP_LOGI(TAG, "[T+%07.3f] JSON de dispositivos emitido", timebase_now_s());
}
