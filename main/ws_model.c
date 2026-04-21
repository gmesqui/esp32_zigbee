#include "ws_model.h"

#include "utils.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ws_json_append(char **p, char *end, const char *fmt, ...)
{
    if (!p || !*p || !end || *p >= end) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    if (n >= end - *p) {
        *p = end;
    } else {
        *p += n;
    }
}

void ws_json_append_string(char **p, char *end, const char *value)
{
    if (!value) {
        value = "";
    }

    ws_json_append(p, end, "\"");
    while (*value && *p < end - 1) {
        unsigned char ch = (unsigned char)*value++;
        switch (ch) {
            case '\\': ws_json_append(p, end, "\\\\"); break;
            case '"':  ws_json_append(p, end, "\\\""); break;
            case '\n': ws_json_append(p, end, "\\n"); break;
            case '\r': ws_json_append(p, end, "\\r"); break;
            case '\t': ws_json_append(p, end, "\\t"); break;
            default:
                if (ch < 0x20) {
                    ws_json_append(p, end, "\\u%04X", ch);
                } else {
                    *(*p)++ = (char)ch;
                    **p = '\0';
                }
                break;
        }
    }
    ws_json_append(p, end, "\"");
}

void ws_model_append_device_id(char **p, char *end, uint64_t ieee)
{
    char ibuf[20];
    utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
    ws_json_append_string(p, end, ibuf);
}

static void append_capability(char **p, char *end, bool *first,
                              const char *capability)
{
    if (!*first) {
        ws_json_append(p, end, ",");
    }
    *first = false;
    ws_json_append_string(p, end, capability);
}

static void append_capabilities(char **p, char *end, const device_record_t *dev)
{
    bool first = true;

    ws_json_append(p, end, "\"capabilities\":[");
    if (dm_has_in_cluster(dev, 0x0006, NULL)) {
        append_capability(p, end, &first, "switch");
    }
    if (dm_has_in_cluster(dev, 0x0008, NULL)) {
        append_capability(p, end, &first, "brightness");
    }
    if (dm_has_in_cluster(dev, 0x0402, NULL)) {
        append_capability(p, end, &first, "temperature_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0405, NULL)) {
        append_capability(p, end, &first, "humidity_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0403, NULL)) {
        append_capability(p, end, &first, "pressure_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0400, NULL)) {
        append_capability(p, end, &first, "illuminance_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0406, NULL)) {
        append_capability(p, end, &first, "occupancy_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0001, NULL)) {
        append_capability(p, end, &first, "battery_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0B04, NULL)) {
        append_capability(p, end, &first, "power_sensor");
    }
    if (dm_has_in_cluster(dev, 0x0500, NULL)) {
        append_capability(p, end, &first, "ias_zone_sensor");
    }
    ws_json_append(p, end, "]");
}

void ws_model_append_inventory_device(char **p, char *end,
                                      const device_record_t *dev)
{
    ws_json_append(p, end, "{");
    ws_json_append(p, end, "\"device_id\":");
    ws_model_append_device_id(p, end, dev->ieee_addr);
    ws_json_append(p, end, ",\"name\":");
    ws_json_append_string(p, end, dm_display_name(dev));
    ws_json_append(p, end, ",\"manufacturer\":");
    ws_json_append_string(p, end, dev->manufacturer);
    ws_json_append(p, end, ",\"model\":");
    ws_json_append_string(p, end, dev->model);
    ws_json_append(p, end, ",\"power_source\":");
    ws_json_append_string(p, end, utils_power_source_name(dev->power_source));
    ws_json_append(p, end, ",");
    append_capabilities(p, end, dev);
    ws_json_append(p, end, "}");
}

void ws_model_append_state_device(char **p, char *end,
                                  const device_record_t *dev)
{
    uint32_t last_seen = dev->last_seen_ms / 1000u;

    ws_json_append(p, end, "{");
    ws_json_append(p, end, "\"device_id\":");
    ws_model_append_device_id(p, end, dev->ieee_addr);
    ws_json_append(p, end, ",\"meta\":{\"reachable\":%s,\"last_seen\":%lu",
                   dev->online ? "true" : "false",
                   (unsigned long)last_seen);
    if (dev->radio_metrics_valid) {
        ws_json_append(p, end, ",\"link_quality\":%u", dev->last_lqi);
    }
    ws_json_append(p, end, "},\"state\":{}");
    ws_json_append(p, end, "}");
}

bool ws_model_attr_meta(uint16_t cluster_id, uint16_t attr_id,
                        ws_attr_meta_t *out)
{
    ws_attr_meta_t meta = {0};

    switch (cluster_id) {
        case 0x0006:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "state", NULL, true };
            break;
        case 0x0008:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "brightness", NULL, true };
            break;
        case 0x0402:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "temperature", "C", true };
            break;
        case 0x0405:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "humidity", "%", true };
            break;
        case 0x0403:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "pressure", "hPa", true };
            break;
        case 0x0400:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "illuminance", "lx", true };
            break;
        case 0x0406:
            if (attr_id == 0x0000) meta = (ws_attr_meta_t){ "occupancy", NULL, true };
            break;
        case 0x0001:
            if (attr_id == 0x0021) meta = (ws_attr_meta_t){ "battery", "%", true };
            if (attr_id == 0x0020) meta = (ws_attr_meta_t){ "voltage", "mV", true };
            break;
        case 0x0B04:
            if (attr_id == 0x050B) meta = (ws_attr_meta_t){ "power", "W", true };
            break;
        case 0x0500:
            if (attr_id == 0x0002) meta = (ws_attr_meta_t){ "contact", NULL, true };
            break;
        default:
            break;
    }

    if (out) {
        *out = meta;
    }
    return meta.known;
}

static uint32_t read_u32_le(const uint8_t value[8], int len)
{
    uint32_t out = 0;
    if (len > 4) {
        len = 4;
    }
    for (int i = 0; i < len; i++) {
        out |= ((uint32_t)value[i] << (8 * i));
    }
    return out;
}

static int zcl_type_size(uint8_t type)
{
    switch (type) {
        case 0x10:
        case 0x18:
        case 0x20:
        case 0x28:
            return 1;
        case 0x19:
        case 0x21:
        case 0x29:
            return 2;
        case 0x2A:
            return 3;
        case 0x23:
        case 0x2B:
            return 4;
        default:
            return 1;
    }
}

static int32_t signed_value(uint32_t raw, int len)
{
    if (len == 1) {
        return (int8_t)(raw & 0xFFu);
    }
    if (len == 2) {
        return (int16_t)(raw & 0xFFFFu);
    }
    return (int32_t)raw;
}

static void append_attr_scalar(char **p, char *end, uint16_t cluster_id,
                               uint16_t attr_id, uint8_t attr_type,
                               const uint8_t value[8])
{
    int len = zcl_type_size(attr_type);
    uint32_t raw = read_u32_le(value, len);
    int32_t sval = signed_value(raw, len);

    switch (cluster_id) {
        case 0x0006:
            ws_json_append_string(p, end, raw ? "ON" : "OFF");
            break;
        case 0x0008:
            ws_json_append(p, end, "%lu", (unsigned long)(raw & 0xFFu));
            break;
        case 0x0402:
            ws_json_append(p, end, "%.2f", (double)sval / 100.0);
            break;
        case 0x0405:
            ws_json_append(p, end, "%.2f", (double)(raw & 0xFFFFu) / 100.0);
            break;
        case 0x0403:
            ws_json_append(p, end, "%.1f", (double)sval / 10.0);
            break;
        case 0x0400:
            ws_json_append(p, end, "%lu", (unsigned long)(raw & 0xFFFFu));
            break;
        case 0x0406:
            ws_json_append(p, end, "%s", (raw & 0x01u) ? "true" : "false");
            break;
        case 0x0001:
            if (attr_id == 0x0020) {
                ws_json_append(p, end, "%lu", (unsigned long)((raw & 0xFFu) * 100u));
            } else {
                ws_json_append(p, end, "%lu", (unsigned long)((raw & 0xFFu) / 2u));
            }
            break;
        case 0x0B04:
            ws_json_append(p, end, "%.1f", (double)sval / 10.0);
            break;
        case 0x0500:
        {
            uint16_t st = (uint16_t)(raw & 0xFFFFu);
            ws_json_append(p, end, "%s", (st & 0x01u) ? "false" : "true");
            break;
        }
        default:
            ws_json_append(p, end, "%lu", (unsigned long)raw);
            break;
    }
}

bool ws_model_append_attr_value_object(char **p, char *end,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       uint8_t attr_type,
                                       const uint8_t value[8],
                                       uint32_t ts)
{
    ws_attr_meta_t meta;
    if (!ws_model_attr_meta(cluster_id, attr_id, &meta)) {
        return false;
    }

    ws_json_append(p, end, "{\"value\":");
    append_attr_scalar(p, end, cluster_id, attr_id, attr_type, value);
    if (meta.unit) {
        ws_json_append(p, end, ",\"unit\":");
        ws_json_append_string(p, end, meta.unit);
    }
    ws_json_append(p, end, ",\"ts\":%lu,\"quality\":\"valid\"}",
                   (unsigned long)ts);
    return true;
}

void ws_model_append_event_change(char **p, char *end, const zb_event_t *evt)
{
    ws_attr_meta_t meta;

    if (!evt || !ws_model_attr_meta(evt->cluster_id, evt->attr_id, &meta)) {
        ws_json_append(p, end, "{}");
        return;
    }

    ws_json_append(p, end, "{");
    ws_json_append_string(p, end, meta.name);
    ws_json_append(p, end, ":");
    ws_model_append_attr_value_object(p, end, evt->cluster_id, evt->attr_id,
                                      evt->attr_type, evt->value,
                                      utils_uptime_ms() / 1000u);
    ws_json_append(p, end, "}");
}

