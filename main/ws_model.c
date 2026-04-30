#include "ws_model.h"

#include "utils.h"
#include "zcl_handler.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool device_may_have_battery(const device_record_t *dev)
{
    if (!dev) {
        return false;
    }
    if (dev->power_source == 0x00) {
        return dev->is_sleepy;
    }
    return utils_power_source_may_have_battery(dev->power_source);
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
    if (dm_has_in_cluster(dev, 0x0001, NULL) && device_may_have_battery(dev)) {
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

static void append_cluster_array(char **p, char *end, const char *name,
                                 const uint16_t *clusters, uint8_t count)
{
    ws_json_append_string(p, end, name);
    ws_json_append(p, end, ":[");
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) {
            ws_json_append(p, end, ",");
        }
        ws_json_append(p, end, "%u", clusters[i]);
    }
    ws_json_append(p, end, "]");
}

static const char *report_cfg_reason(uint8_t result)
{
    switch ((report_cfg_result_t)result) {
        case REPORT_CFG_RESULT_FAIL:
            return "fail";
        case REPORT_CFG_RESULT_MISSING:
            return "missing";
        case REPORT_CFG_RESULT_BIND_FAIL:
            return "bind_fail";
        case REPORT_CFG_RESULT_WRITE_FAIL:
            return "write_fail";
        case REPORT_CFG_RESULT_UNSUPPORTED:
            return "unsupported";
        default:
            return "unknown";
    }
}

static void append_reporting_failures(char **p, char *end,
                                      const device_record_t *dev)
{
    bool first = true;

    ws_json_append(p, end, "\"failures\":[");
    for (uint8_t i = 0; i < dev->report_cfg_record_count; i++) {
        const report_cfg_record_t *record = &dev->report_cfg_records[i];
        if (record->result != REPORT_CFG_RESULT_FAIL &&
            record->result != REPORT_CFG_RESULT_MISSING &&
            record->result != REPORT_CFG_RESULT_BIND_FAIL &&
            record->result != REPORT_CFG_RESULT_WRITE_FAIL &&
            record->result != REPORT_CFG_RESULT_UNSUPPORTED) {
            continue;
        }
        if (!first) {
            ws_json_append(p, end, ",");
        }
        first = false;
        ws_json_append(p, end,
                       "{\"endpoint\":%u,\"cluster_id\":%u,"
                       "\"attr_id\":%u,\"status\":%u,\"reason\":",
                       record->endpoint, record->cluster_id,
                       record->attr_id, record->status);
        ws_json_append_string(p, end, report_cfg_reason(record->result));
        ws_json_append(p, end, ",\"cluster_name\":");
        ws_json_append_string(p, end, utils_cluster_name(record->cluster_id));
        ws_json_append(p, end, "}");
    }
    ws_json_append(p, end, "],\"overflow\":%s",
                   dev->report_cfg_record_overflow ? "true" : "false");
}

static void append_endpoints(char **p, char *end, const device_record_t *dev)
{
    ws_json_append(p, end, "\"endpoints\":[");
    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        const endpoint_record_t *ep = &dev->endpoints[i];
        if (i > 0) {
            ws_json_append(p, end, ",");
        }
        ws_json_append(p, end,
                       "{\"id\":%u,\"profile_id\":%u,\"device_id\":%u,"
                       "\"device_type\":",
                       ep->endpoint_id, ep->profile_id, ep->device_id);
        ws_json_append_string(p, end, utils_device_type_name(ep->device_id));
        ws_json_append(p, end, ",");
        append_cluster_array(p, end, "in_clusters",
                             ep->in_clusters, ep->in_cluster_count);
        ws_json_append(p, end, ",");
        append_cluster_array(p, end, "out_clusters",
                             ep->out_clusters, ep->out_cluster_count);
        ws_json_append(p, end, "}");
    }
    ws_json_append(p, end, "]");
}

void ws_model_append_inventory_device(char **p, char *end,
                                      const device_record_t *dev)
{
    uint32_t last_seen = dev->last_seen_ms / 1000u;

    ws_json_append(p, end, "{");
    ws_json_append(p, end, "\"device_id\":");
    ws_model_append_device_id(p, end, dev->ieee_addr);
    ws_json_append(p, end, ",\"name\":");
    ws_json_append_string(p, end, dm_display_name(dev));
    ws_json_append(p, end, ",\"nwk\":%u,\"online\":%s,\"is_sleepy\":%s,"
                   "\"state\":",
                   dev->nwk_addr, dev->online ? "true" : "false",
                   dev->is_sleepy ? "true" : "false");
    ws_json_append_string(p, end, utils_device_state_name((int)dev->state));
    ws_json_append(p, end, ",\"manufacturer\":");
    ws_json_append_string(p, end, dev->manufacturer);
    ws_json_append(p, end, ",\"model\":");
    ws_json_append_string(p, end, dev->model);
    ws_json_append(p, end, ",\"power_source\":");
    ws_json_append_string(p, end, utils_power_source_name(dev->power_source));
    ws_json_append(p, end,
                   ",\"last_seen_s\":%lu,\"reporting\":{"
                   "\"configured\":%s,\"in_progress\":%s,"
                   "\"expected\":%u,\"received\":%u,\"failed\":%u,",
                   (unsigned long)last_seen,
                   dev->reporting_configured ? "true" : "false",
                   dev->report_cfg_in_progress ? "true" : "false",
                   dev->report_cfg_expected,
                   dev->report_cfg_received,
                   dev->report_cfg_failed);
    append_reporting_failures(p, end, dev);
    ws_json_append(p, end,
                   "},\"stats\":{\"report_attr_ok\":%lu,"
                   "\"report_attr_unchanged\":%lu,"
                   "\"read_rsp_ok\":%lu,\"read_rsp_fail\":%lu,"
                   "\"interview_attempts\":%lu}",
                   (unsigned long)dev->report_attr_ok,
                   (unsigned long)dev->report_attr_unchanged,
                   (unsigned long)dev->read_rsp_ok,
                   (unsigned long)dev->read_rsp_fail,
                   (unsigned long)dev->interview_attempts);
    if (dev->radio_metrics_valid) {
        ws_json_append(p, end, ",\"lqi\":%u,\"rssi\":%d",
                       dev->last_lqi, (int)dev->last_rssi);
    }
    ws_json_append(p, end, ",");
    append_endpoints(p, end, dev);
    ws_json_append(p, end, ",");
    append_capabilities(p, end, dev);
    ws_json_append(p, end, "}");
}

void ws_model_append_state_device(char **p, char *end,
                                  const device_record_t *dev)
{
    uint32_t last_seen = dev->last_seen_ms / 1000u;
    attr_cache_entry_t *attrs = calloc(MAX_ATTR_CACHE, sizeof(*attrs));
    size_t attr_count = 0;

    if (attrs) {
        size_t total = zcl_get_cached_attrs(dev->ieee_addr, attrs,
                                            MAX_ATTR_CACHE);
        attr_count = total > MAX_ATTR_CACHE ? MAX_ATTR_CACHE : total;
    }

    ws_json_append(p, end, "{");
    ws_json_append(p, end, "\"device_id\":");
    ws_model_append_device_id(p, end, dev->ieee_addr);
    ws_json_append(p, end, ",\"meta\":{\"reachable\":%s,\"last_seen\":%lu",
                   dev->online ? "true" : "false",
                   (unsigned long)last_seen);
    if (dev->radio_metrics_valid) {
        ws_json_append(p, end, ",\"link_quality\":%u", dev->last_lqi);
    }
    ws_json_append(p, end, "},\"state\":{");

    bool first = true;
    for (size_t i = 0; i < attr_count; i++) {
        ws_attr_meta_t meta;
        attr_cache_entry_t *attr = &attrs[i];

        if (!ws_model_attr_meta(attr->cluster_id, attr->attr_id, &meta)) {
            continue;
        }
        if (!first) {
            ws_json_append(p, end, ",");
        }
        first = false;
        ws_json_append_string(p, end, meta.name);
        ws_json_append(p, end, ":");
        ws_model_append_attr_value_object(p, end, attr->cluster_id,
                                          attr->attr_id, attr->attr_type,
                                          attr->value,
                                          attr->last_update_ms / 1000u);
    }

    ws_json_append(p, end, "}");
    ws_json_append(p, end, "}");
    free(attrs);
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
