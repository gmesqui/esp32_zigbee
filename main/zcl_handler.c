#include "zcl_handler.h"
#include "zb_events.h"
#include "device_manager.h"
#include "device_interview.h"
#include "led_driver.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "zb_osif_platform.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_common.h"

// ---------------------------------------------------------------------------
// Attribute value cache
// ---------------------------------------------------------------------------

static attr_cache_entry_t  g_attr_cache[MAX_ATTR_CACHE];
static pending_attr_t      g_pending[MAX_PENDING_ATTRS];
static SemaphoreHandle_t   g_attr_mutex;

#define MAX_UNSUPPORTED_ATTRS  64

typedef struct {
    uint64_t ieee_addr;
    uint8_t  endpoint_id;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint32_t last_update_ms;
    bool     in_use;
} unsupported_attr_entry_t;

static unsupported_attr_entry_t g_unsupported_attr_cache[MAX_UNSUPPORTED_ATTRS];

void zcl_handler_init(void)
{
    memset(g_attr_cache, 0, sizeof(g_attr_cache));
    memset(g_pending,    0, sizeof(g_pending));
    memset(g_unsupported_attr_cache, 0, sizeof(g_unsupported_attr_cache));
    g_attr_mutex = xSemaphoreCreateMutex();
    configASSERT(g_attr_mutex);
}

// ---------------------------------------------------------------------------
// Cache helpers
// ---------------------------------------------------------------------------

static attr_cache_entry_t *cache_find(uint64_t ieee, uint8_t ep,
                                       uint16_t cluster, uint16_t attr_id)
{
    for (int i = 0; i < MAX_ATTR_CACHE; i++) {
        attr_cache_entry_t *e = &g_attr_cache[i];
        if (e->in_use && e->ieee_addr == ieee && e->endpoint_id == ep &&
            e->cluster_id == cluster && e->attr_id == attr_id)
            return e;
    }
    return NULL;
}

static attr_cache_entry_t *cache_alloc(void)
{
    for (int i = 0; i < MAX_ATTR_CACHE; i++) {
        if (!g_attr_cache[i].in_use) return &g_attr_cache[i];
    }
    // Evict oldest entry
    uint32_t oldest_ms = UINT32_MAX;
    attr_cache_entry_t *oldest = &g_attr_cache[0];
    for (int i = 0; i < MAX_ATTR_CACHE; i++) {
        if (g_attr_cache[i].last_update_ms < oldest_ms) {
            oldest_ms = g_attr_cache[i].last_update_ms;
            oldest = &g_attr_cache[i];
        }
    }
    return oldest;
}

static unsupported_attr_entry_t *unsupported_find(uint64_t ieee, uint8_t ep,
                                                  uint16_t cluster,
                                                  uint16_t attr_id)
{
    for (int i = 0; i < MAX_UNSUPPORTED_ATTRS; i++) {
        unsupported_attr_entry_t *e = &g_unsupported_attr_cache[i];
        if (e->in_use && e->ieee_addr == ieee && e->endpoint_id == ep &&
            e->cluster_id == cluster && e->attr_id == attr_id) {
            return e;
        }
    }
    return NULL;
}

static unsupported_attr_entry_t *unsupported_alloc(void)
{
    for (int i = 0; i < MAX_UNSUPPORTED_ATTRS; i++) {
        if (!g_unsupported_attr_cache[i].in_use) {
            return &g_unsupported_attr_cache[i];
        }
    }

    uint32_t oldest_ms = UINT32_MAX;
    unsupported_attr_entry_t *oldest = &g_unsupported_attr_cache[0];
    for (int i = 0; i < MAX_UNSUPPORTED_ATTRS; i++) {
        if (g_unsupported_attr_cache[i].last_update_ms < oldest_ms) {
            oldest_ms = g_unsupported_attr_cache[i].last_update_ms;
            oldest = &g_unsupported_attr_cache[i];
        }
    }
    return oldest;
}

static bool unsupported_has_attr_locked(uint64_t ieee, uint8_t ep,
                                        uint16_t cluster, uint16_t attr_id)
{
    return unsupported_find(ieee, ep, cluster, attr_id) != NULL;
}

static bool unsupported_has_attr(uint64_t ieee, uint8_t ep,
                                 uint16_t cluster, uint16_t attr_id)
{
    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    bool has_attr = unsupported_has_attr_locked(ieee, ep, cluster, attr_id);
    xSemaphoreGive(g_attr_mutex);
    return has_attr;
}

static void unsupported_mark_attr(uint64_t ieee, uint8_t ep,
                                  uint16_t cluster, uint16_t attr_id)
{
    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    unsupported_attr_entry_t *e = unsupported_find(ieee, ep, cluster, attr_id);
    if (!e) {
        e = unsupported_alloc();
        e->ieee_addr = ieee;
        e->endpoint_id = ep;
        e->cluster_id = cluster;
        e->attr_id = attr_id;
        e->in_use = true;
    }
    e->last_update_ms = utils_uptime_ms();
    xSemaphoreGive(g_attr_mutex);
}

static void unsupported_clear_attr_locked(uint64_t ieee, uint8_t ep,
                                          uint16_t cluster, uint16_t attr_id)
{
    unsupported_attr_entry_t *e = unsupported_find(ieee, ep, cluster, attr_id);
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

static void unsupported_clear_device_locked(uint64_t ieee)
{
    for (int i = 0; i < MAX_UNSUPPORTED_ATTRS; i++) {
        unsupported_attr_entry_t *e = &g_unsupported_attr_cache[i];
        if (e->in_use && e->ieee_addr == ieee) {
            memset(e, 0, sizeof(*e));
        }
    }
}

static void attr_cache_clear_device_locked(uint64_t ieee)
{
    for (int i = 0; i < MAX_ATTR_CACHE; i++) {
        attr_cache_entry_t *e = &g_attr_cache[i];
        if (e->in_use && e->ieee_addr == ieee) {
            memset(e, 0, sizeof(*e));
        }
    }
}

/** Store or update a cached value. Returns true if the value changed.
 *  Caller must hold g_attr_mutex. */
static bool cache_update(uint64_t ieee, uint8_t ep, uint16_t cluster,
                          uint16_t attr_id, uint8_t attr_type,
                          const uint8_t *val, uint8_t val_len)
{
    if (val_len > 8) val_len = 8;

    attr_cache_entry_t *e = cache_find(ieee, ep, cluster, attr_id);
    bool changed = true;

    if (e) {
        changed = (memcmp(e->value, val, val_len) != 0);
    } else {
        e = cache_alloc();
        e->ieee_addr    = ieee;
        e->endpoint_id  = ep;
        e->cluster_id   = cluster;
        e->attr_id      = attr_id;
        e->attr_type    = attr_type;
        e->in_use       = true;
        changed         = true;    // first time is always "changed"
    }

    memcpy(e->value, val, val_len);
    e->last_update_ms = utils_uptime_ms();
    return changed;
}

// ---------------------------------------------------------------------------
// Value decoding helpers
// ---------------------------------------------------------------------------

static uint32_t read_uint(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v |= ((uint32_t)p[i] << (8 * i));
    return v;
}

static int32_t read_int(const uint8_t *p, int bytes)
{
    int32_t v = (int32_t)read_uint(p, bytes);
    // Sign-extend
    if (bytes < 4) {
        int shift = 32 - 8 * bytes;
        v = (v << shift) >> shift;
    }
    return v;
}

// Return value size in bytes for common ZCL types
static int zcl_type_size(uint8_t type)
{
    switch (type) {
        case 0x10: return 1;  // boolean
        case 0x18: case 0x20: return 1;  // bitmap8, uint8
        case 0x19: case 0x21: return 2;  // bitmap16, uint16
        case 0x28: return 1;  // int8
        case 0x29: return 2;  // int16
        case 0x2A: return 3;  // int24
        case 0x2B: return 4;  // int32
        case 0x22: return 3;  // uint24
        case 0x23: return 4;  // uint32
        default:   return 1;
    }
}

static bool lookup_neighbor_metrics(uint16_t nwk_addr, uint8_t *lqi_out, int8_t *rssi_out)
{
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    esp_zb_nwk_neighbor_info_t nbr = {0};

    while (esp_zb_nwk_get_next_neighbor(&it, &nbr) == ESP_OK) {
        if (nbr.short_addr != nwk_addr) {
            continue;
        }

        if (lqi_out) {
            *lqi_out = nbr.lqi;
        }
        if (rssi_out) {
            *rssi_out = nbr.rssi;
        }
        return true;
    }

    return false;
}

typedef struct {
    uint16_t cluster_id;
    uint8_t  attr_count;
    uint16_t attrs[3];
} sleepy_probe_entry_t;

static const sleepy_probe_entry_t k_sleepy_probe_table[] = {
    { 0x0001, 2, { 0x0020, 0x0021 } },         // Battery voltage, battery %
    { 0x0006, 1, { 0x0000 } },                 // On/Off
    { 0x0008, 1, { 0x0000 } },                 // Level
    { 0x0300, 3, { 0x0000, 0x0001, 0x0007 } }, // Color control
    { 0x0402, 1, { 0x0000 } },                 // Temperature
    { 0x0405, 1, { 0x0000 } },                 // Humidity
    { 0x0403, 1, { 0x0000 } },                 // Pressure
    { 0x0400, 1, { 0x0000 } },                 // Illuminance
    { 0x0406, 1, { 0x0000 } },                 // Occupancy
    { 0x0500, 1, { 0x0002 } },                 // IAS Zone status
    { 0x0B04, 1, { 0x050B } },                 // Active power
};

static void send_read_attrs(uint16_t nwk_addr, uint8_t ep_id,
                            uint16_t cluster_id, uint8_t attr_count,
                            uint16_t *attrs)
{
    char attr_buf[96];
    size_t used = 0;

    if (!attrs || attr_count == 0) {
        return;
    }

    attr_buf[0] = '\0';
    for (uint8_t i = 0; i < attr_count; i++) {
        int n = snprintf(attr_buf + used, sizeof(attr_buf) - used, "%s0x%04X",
                         i ? "," : "", attrs[i]);
        if (n < 0 || (size_t)n >= sizeof(attr_buf) - used) {
            break;
        }
        used += (size_t)n;
    }

    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint          = 1,
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint          = ep_id,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID    = cluster_id,
        .attr_number  = attr_count,
        .attr_field   = attrs,
    };
    ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s read_attrs=[%s]",
           nwk_addr, ep_id, utils_cluster_name(cluster_id), attr_buf);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

static bool cache_has_attr(uint64_t ieee, uint8_t ep, uint16_t cluster, uint16_t attr_id)
{
    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    bool has_attr = (cache_find(ieee, ep, cluster, attr_id) != NULL);
    xSemaphoreGive(g_attr_mutex);
    return has_attr;
}

static const endpoint_record_t *find_endpoint_record(const device_record_t *dev, uint8_t endpoint_id)
{
    if (!dev) {
        return NULL;
    }

    for (int i = 0; i < dev->endpoint_count; i++) {
        if (dev->endpoints[i].endpoint_id == endpoint_id) {
            return &dev->endpoints[i];
        }
    }

    return NULL;
}

static bool endpoint_has_in_cluster(const endpoint_record_t *ep, uint16_t cluster_id)
{
    if (!ep) {
        return false;
    }

    for (int i = 0; i < ep->in_cluster_count; i++) {
        if (ep->in_clusters[i] == cluster_id) {
            return true;
        }
    }

    return false;
}

static bool is_recent_probe(const device_record_t *dev)
{
    if (!dev || dev->last_probe_ms == 0) {
        return false;
    }
    return (utils_uptime_ms() - dev->last_probe_ms) < 5000u;
}

static void maybe_probe_sleepy_device(device_record_t *dev, uint8_t ep,
                                      uint16_t reported_cluster, uint16_t reported_attr)
{
    if (!dev || !dev->is_sleepy || ep == 0) {
        return;
    }

    const endpoint_record_t *endpoint = find_endpoint_record(dev, ep);
    if (!endpoint) {
        return;
    }

    uint32_t now = utils_uptime_ms();
    if ((now - dev->last_probe_ms) < 2000u) {
        return;
    }

    bool sent_any_probe = false;
    for (size_t i = 0; i < sizeof(k_sleepy_probe_table) / sizeof(k_sleepy_probe_table[0]); i++) {
        const sleepy_probe_entry_t *entry = &k_sleepy_probe_table[i];
        if (!endpoint_has_in_cluster(endpoint, entry->cluster_id)) {
            continue;
        }

        uint16_t missing_attrs[3] = {0};
        uint8_t missing_count = 0;
        for (int a = 0; a < entry->attr_count; a++) {
            uint16_t attr_id = entry->attrs[a];
            if (!cache_has_attr(dev->ieee_addr, ep, entry->cluster_id, attr_id) &&
                !unsupported_has_attr(dev->ieee_addr, ep, entry->cluster_id, attr_id)) {
                missing_attrs[missing_count++] = attr_id;
            }
        }

        if (missing_count == 0) {
            continue;
        }

        if (!sent_any_probe) {
            dev->last_probe_ms = now;
            sent_any_probe = true;
        }

        ZB_LOG("SLEEPY_PROBE %s read cluster=%s ep=%u missing=%u",
               dm_display_name(dev), utils_cluster_name(entry->cluster_id),
               ep, missing_count);
        send_read_attrs(dev->nwk_addr, ep, entry->cluster_id, missing_count, missing_attrs);
    }

    if (!sent_any_probe && !dev->radio_metrics_valid) {
        uint16_t attr = reported_attr;
        dev->last_probe_ms = now;
        ZB_LOG("SLEEPY_PROBE %s read cluster=%s attr=0x%04X ep=%u for metrics",
               dm_display_name(dev), utils_cluster_name(reported_cluster),
               reported_attr, ep);
        send_read_attrs(dev->nwk_addr, ep, reported_cluster, 1, &attr);
    }
}

// ---------------------------------------------------------------------------
// Format a decoded value as a human-readable string for STATE logs
// ---------------------------------------------------------------------------

static void format_attr_value(uint16_t cluster, uint16_t attr_id,
                               uint8_t attr_type, const uint8_t *val,
                               char *out, size_t out_len)
{
    int sz = zcl_type_size(attr_type);

    switch (cluster) {
        case 0x0006:  // On/Off
            if (attr_id == 0x0000) {
                snprintf(out, out_len, "on_off=%s", val[0] ? "ON" : "OFF");
            } else {
                snprintf(out, out_len, "on_off[0x%04X]=0x%02X", attr_id, val[0]);
            }
            return;

        case 0x0008:  // Level Control
            snprintf(out, out_len, "level=%u (%.0f%%)",
                     val[0], val[0] / 2.54f);
            return;

        case 0x0402:  // Temperature
            snprintf(out, out_len, "temperature=%.2f C",
                     (float)read_int(val, sz) / 100.0f);
            return;

        case 0x0405:  // Humidity
            snprintf(out, out_len, "humidity=%.2f %%",
                     (float)read_uint(val, sz) / 100.0f);
            return;

        case 0x0403:  // Pressure
            snprintf(out, out_len, "pressure=%.1f kPa",
                     (float)read_int(val, sz) / 10.0f);
            return;

        case 0x0400:  // Illuminance
        {
            uint16_t raw = (uint16_t)read_uint(val, 2);
            if (raw == 0) {
                snprintf(out, out_len, "illuminance=0 lux");
            } else {
                float lux = powf(10.0f, (raw - 1) / 10000.0f);
                snprintf(out, out_len, "illuminance=%.1f lux (raw=%u)", lux, raw);
            }
            return;
        }

        case 0x0406:  // Occupancy
            snprintf(out, out_len, "occupancy=%s",
                     (val[0] & 0x01) ? "OCCUPIED" : "CLEAR");
            return;

        case 0x0001:  // Power Config
            if (attr_id == 0x0020) {
                snprintf(out, out_len, "voltage=%u mV",
                         (unsigned)read_uint(val, 1) * 100u);
            } else if (attr_id == 0x0021) {
                uint8_t raw = val[0];
                if (raw == 0xFF) {
                    snprintf(out, out_len, "battery=unknown");
                } else {
                    snprintf(out, out_len, "battery=%u %%", raw / 2u);
                }
            } else {
                snprintf(out, out_len, "power_cfg[0x%04X]=0x%02X", attr_id, val[0]);
            }
            return;

        case 0x0500:  // IAS Zone
            if (attr_id == 0x0002) {
                uint16_t status = (uint16_t)read_uint(val, 2);
                char flags[64] = "";
                if (status & 0x0001) strcat(flags, "ALARM1 ");
                if (status & 0x0002) strcat(flags, "ALARM2 ");
                if (status & 0x0004) strcat(flags, "TAMPER ");
                if (status & 0x0008) strcat(flags, "BATT_LOW ");
                if (flags[0] == '\0') strcat(flags, "CLEAR");
                snprintf(out, out_len, "ias_zone=0x%04X [%s]", status, flags);
            } else {
                snprintf(out, out_len, "ias[0x%04X]=0x%02X", attr_id, val[0]);
            }
            return;

        default:
            snprintf(out, out_len, "cluster=0x%04X attr=0x%04X val=0x",
                     cluster, attr_id);
            for (int i = 0; i < sz && i < 4; i++) {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02X", val[i]);
                strncat(out, hex, out_len - strlen(out) - 1);
            }
            return;
    }
}

// ---------------------------------------------------------------------------
// Internal: process one attribute from any source
// ---------------------------------------------------------------------------

static void emit_attr_changed_event(const device_record_t *dev, uint8_t ep,
                                     uint16_t cluster, uint16_t attr_id,
                                     uint8_t attr_type, const uint8_t *val,
                                     int sz)
{
    zb_event_t evt = {
        .type       = ZB_EVT_ATTR_CHANGED,
        .ieee       = dev->ieee_addr,
        .lqi        = dev->last_lqi,
        .has_lqi    = dev->radio_metrics_valid,
        .endpoint   = ep,
        .cluster_id = cluster,
        .attr_id    = attr_id,
        .attr_type  = attr_type,
    };
    strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
    evt.friendly_name[ZB_EVT_NAME_LEN - 1] = '\0';
    if (sz > 0 && sz <= 8) {
        memcpy(evt.value, val, (size_t)sz);
    }
    zb_events_emit(&evt);
}

static bool process_attribute(device_record_t *dev, uint8_t ep,
                               uint16_t cluster, uint16_t attr_id,
                               uint8_t attr_type, const void *raw_val,
                               bool emit_event)
{
    int sz = zcl_type_size(attr_type);
    const uint8_t *val = (const uint8_t *)raw_val;

    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    bool changed = cache_update(dev->ieee_addr, ep, cluster,
                                 attr_id, attr_type, val, (uint8_t)sz);
    unsupported_clear_attr_locked(dev->ieee_addr, ep, cluster, attr_id);
    xSemaphoreGive(g_attr_mutex);

    if (changed) {
        dev->report_attr_ok++;
        char desc[96];
        format_attr_value(cluster, attr_id, attr_type, val, desc, sizeof(desc));
        ZB_LOG("STATE %s/%u %s", dm_display_name(dev), ep, desc);

        // Emit event for registered consumers - non-blocking
        if (emit_event) {
            emit_attr_changed_event(dev, ep, cluster, attr_id, attr_type, val, sz);
        }
    } else {
        dev->report_attr_unchanged++;
    }

    // Update Basic cluster identity fields
    if (cluster == 0x0000) {
        if (attr_id == 0x0004 && attr_type == 0x42) {
            // manufacturer string (ZCL octet string: first byte = length)
            uint8_t slen = val[0];
            if (slen > 32) slen = 32;
            char next[33] = {0};
            memcpy(next, val + 1, slen);
            if (strcmp(dev->manufacturer, next) != 0) {
                memcpy(dev->manufacturer, next, sizeof(dev->manufacturer));
                dev->dirty = true;
            }
        } else if (attr_id == 0x0005 && attr_type == 0x42) {
            uint8_t slen = val[0];
            if (slen > 32) slen = 32;
            char next[33] = {0};
            memcpy(next, val + 1, slen);
            if (strcmp(dev->model, next) != 0) {
                memcpy(dev->model, next, sizeof(dev->model));
                dev->dirty = true;
            }
        } else if (attr_id == 0x0007) {
            if (dev->power_source != val[0]) {
                dev->power_source = val[0];
                dev->dirty = true;
            }
        }
    }

    return changed;
}

// ---------------------------------------------------------------------------
// Public: process Report Attributes
// ---------------------------------------------------------------------------

esp_err_t zcl_on_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    led_trigger_activity_pulse();

    uint16_t src_nwk = msg->src_address.u.short_addr;
    uint8_t  src_ep  = msg->src_endpoint;
    uint16_t cluster = msg->cluster;  // report_attr_message has direct fields

    // Log RAW including a decoded value preview for faster diagnosis.
    char raw_desc[96];
    format_attr_value(cluster, msg->attribute.id, msg->attribute.data.type,
                      msg->attribute.data.value, raw_desc, sizeof(raw_desc));
    ZB_LOG("RX RAW src=0x%04X ep=%u cluster=%s attr=0x%04X type=0x%02X %s",
           src_nwk, src_ep, utils_cluster_name(cluster),
           msg->attribute.id, msg->attribute.data.type, raw_desc);

    // Find device
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    if (!dev) {
        // Unknown short address — buffer and trigger IEEE resolution
        int sz = zcl_type_size(msg->attribute.data.type);
        zcl_pending_attr_save(src_nwk, src_ep, cluster,
                               msg->attribute.id, msg->attribute.data.type,
                               msg->attribute.data.value, (uint8_t)sz);
        // Trigger IEEE resolution via interview module
        di_trigger_ieee_resolve(src_nwk);
        return ESP_OK;
    }

    // Attribute reports do not expose RSSI in this callback, so refresh
    // metrics from the NWK neighbor table when possible (useful for sleepy EDs).
    uint8_t lqi = 0;
    int8_t rssi = 0;
    if (lookup_neighbor_metrics(src_nwk, &lqi, &rssi)) {
        dm_touch(dev, lqi, rssi);
    } else {
        dm_touch(dev, 0, 0);
    }

    // Decode and log
    ZB_LOG("RX DECODE src=%s/%u cluster=%s attr=0x%04X",
           dm_display_name(dev), src_ep,
           utils_cluster_name(cluster), msg->attribute.id);

    process_attribute(dev, src_ep, cluster,
                      msg->attribute.id, msg->attribute.data.type,
                      msg->attribute.data.value, true);

    maybe_probe_sleepy_device(dev, src_ep, cluster, msg->attribute.id);

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public: process Read Attributes Response
// ---------------------------------------------------------------------------

esp_err_t zcl_on_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    led_trigger_activity_pulse();

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    if (!dev) return ESP_OK;

    dm_touch(dev, esp_zb_rssi_to_lqi(msg->info.header.rssi), msg->info.header.rssi);

    const esp_zb_zcl_read_attr_resp_variable_t *var = msg->variables;
    bool any_changed = false;
    uint16_t last_attr_id = 0;
    uint8_t last_attr_type = 0;
    uint8_t last_value[8] = {0};
    int last_size = 0;
    while (var) {
        if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS) {
            int sz = zcl_type_size(var->attribute.data.type);
            if (process_attribute(dev, msg->info.src_endpoint, msg->info.cluster,
                                  var->attribute.id, var->attribute.data.type,
                                  var->attribute.data.value, false)) {
                any_changed = true;
                last_attr_id = var->attribute.id;
                last_attr_type = var->attribute.data.type;
                last_size = sz > 8 ? 8 : sz;
                if (last_size > 0) {
                    memcpy(last_value, var->attribute.data.value, (size_t)last_size);
                }
            }
            dev->read_rsp_ok++;
        } else {
            dev->read_rsp_fail++;
            ZB_LOG("RX READ_ATTR_RSP %s attr=0x%04X status=0x%02X (fail)",
                   dm_display_name(dev), var->attribute.id, var->status);
            if (var->status == ESP_ZB_ZCL_STATUS_UNSUP_ATTRIB) {
                unsupported_mark_attr(dev->ieee_addr, msg->info.src_endpoint,
                                      msg->info.cluster, var->attribute.id);
            }
            if (is_recent_probe(dev)) {
                ZB_LOG("SLEEPY_PROBE %s attr=0x%04X failed status=0x%02X",
                       dm_display_name(dev), var->attribute.id, var->status);
            }
        }
        var = var->next;
    }

    if (is_recent_probe(dev)) {
        bool has_battery = cache_has_attr(dev->ieee_addr, msg->info.src_endpoint, 0x0001, 0x0021);
        bool has_voltage = cache_has_attr(dev->ieee_addr, msg->info.src_endpoint, 0x0001, 0x0020);
        bool battery_unsupported = unsupported_has_attr(dev->ieee_addr, msg->info.src_endpoint,
                                                        0x0001, 0x0021);
        bool voltage_unsupported = unsupported_has_attr(dev->ieee_addr, msg->info.src_endpoint,
                                                        0x0001, 0x0020);
        ZB_LOG("SLEEPY_PROBE %s response cluster=%s rssi=%d lqi=%u battery=%s voltage=%s",
               dm_display_name(dev), utils_cluster_name(msg->info.cluster),
               msg->info.header.rssi, dev->last_lqi,
               has_battery ? "yes" : (battery_unsupported ? "unsupported" : "no"),
               has_voltage ? "yes" : (voltage_unsupported ? "unsupported" : "no"));
    }

    if (any_changed) {
        emit_attr_changed_event(dev, msg->info.src_endpoint, msg->info.cluster,
                                last_attr_id, last_attr_type, last_value, last_size);
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public: IAS Zone Enroll Request
// ---------------------------------------------------------------------------

esp_err_t zcl_on_ias_enroll_req(const esp_zb_zcl_ias_zone_enroll_request_message_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    led_trigger_activity_pulse();

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);

    ZB_LOG("IAS ENROLL_REQ src=0x%04X zone_type=0x%04X mfr=0x%04X",
           src_nwk, msg->zone_type, msg->manufacturer_code);

    // Send enrollment response
    esp_zb_zcl_ias_zone_enroll_response_cmd_t resp = {
        .zcl_basic_cmd = {
            .src_endpoint = 1,
            .dst_addr_u.addr_short = src_nwk,
            .dst_endpoint = msg->info.src_endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS,
        .zone_id = 1,
    };
    esp_zb_zcl_ias_zone_enroll_cmd_resp(&resp);

    if (dev) {
        uint8_t lqi = 0;
        int8_t rssi = 0;
        if (lookup_neighbor_metrics(src_nwk, &lqi, &rssi)) {
            dm_touch(dev, lqi, rssi);
        } else {
            dm_touch(dev, 0, 0);
        }
        ZB_LOG("IAS ENROLL_RSP sent to %s zone_id=1", dm_display_name(dev));
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public: IAS Zone Status Change Notification
// ---------------------------------------------------------------------------

esp_err_t zcl_on_ias_zone_status(
    const esp_zb_zcl_ias_zone_status_change_notification_message_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    led_trigger_activity_pulse();

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    if (!dev) return ESP_OK;

    uint8_t lqi = 0;
    int8_t rssi = 0;
    if (lookup_neighbor_metrics(src_nwk, &lqi, &rssi)) {
        dm_touch(dev, lqi, rssi);
    } else {
        dm_touch(dev, 0, 0);
    }

    uint16_t status = msg->zone_status;
    uint8_t  val[2] = { (uint8_t)(status & 0xFF), (uint8_t)(status >> 8) };
    process_attribute(dev, msg->info.src_endpoint, 0x0500,
                      0x0002, 0x19 /*bitmap16*/, val, true);

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public: Default Response (log errors)
// ---------------------------------------------------------------------------

esp_err_t zcl_on_default_resp(const esp_zb_zcl_cmd_default_resp_message_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    if (msg->status_code != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ZB_LOG("ZCL DEFAULT_RSP src=0x%04X cmd=0x%02X status=0x%02X",
               msg->info.src_address.u.short_addr,
               msg->resp_to_cmd, msg->status_code);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Pending attr buffer (for unknown short addresses)
// ---------------------------------------------------------------------------

void zcl_pending_attr_save(uint16_t nwk_addr, uint8_t ep,
                            uint16_t cluster, uint16_t attr_id,
                            uint8_t attr_type, const void *value, uint8_t val_len)
{
    // Find a free slot (or overwrite oldest)
    pending_attr_t *slot = NULL;
    uint32_t oldest_ts = UINT32_MAX;
    pending_attr_t *oldest = &g_pending[0];

    for (int i = 0; i < MAX_PENDING_ATTRS; i++) {
        if (!g_pending[i].in_use) { slot = &g_pending[i]; break; }
        if (g_pending[i].timestamp_ms < oldest_ts) {
            oldest_ts = g_pending[i].timestamp_ms;
            oldest = &g_pending[i];
        }
    }
    if (!slot) slot = oldest;

    slot->nwk_addr    = nwk_addr;
    slot->endpoint    = ep;
    slot->cluster_id  = cluster;
    slot->attr_id     = attr_id;
    slot->attr_type   = attr_type;
    slot->timestamp_ms= utils_uptime_ms();
    slot->in_use      = true;
    if (val_len > 8) val_len = 8;
    memcpy(slot->value, value, val_len);
}

void zcl_pending_attr_replay(uint64_t ieee, uint16_t nwk_addr)
{
    device_record_t *dev = dm_find_by_ieee(ieee);
    if (!dev) return;

    for (int i = 0; i < MAX_PENDING_ATTRS; i++) {
        pending_attr_t *p = &g_pending[i];
        if (!p->in_use || p->nwk_addr != nwk_addr) continue;

        ZB_LOG("REPLAY pending attr cluster=%s attr=0x%04X for %s",
               utils_cluster_name(p->cluster_id), p->attr_id,
               dm_display_name(dev));

        process_attribute(dev, p->endpoint, p->cluster_id,
                          p->attr_id, p->attr_type, p->value, true);
        p->in_use = false;
    }
}

void zcl_clear_unsupported_attrs(uint64_t ieee)
{
    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    unsupported_clear_device_locked(ieee);
    xSemaphoreGive(g_attr_mutex);
}
void zcl_forget_device(uint64_t ieee)
{
    xSemaphoreTake(g_attr_mutex, portMAX_DELAY);
    attr_cache_clear_device_locked(ieee);
    unsupported_clear_device_locked(ieee);
    xSemaphoreGive(g_attr_mutex);
}
