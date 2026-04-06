#include "mqtt_bridge.h"
#include "zb_events.h"
#include "mqtt_manager.h"
#include "device_manager.h"
#include "device_definition.h"
#include "report_config.h"
#include "zcl_handler.h"
#include "button_handler.h"
#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/task.h"
#include "esp_zigbee_core.h"   // esp_zb_get_current_channel, esp_zb_get_pan_id
#include "esp_heap_caps.h"
#include "sdkconfig.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define BASE    MQTT_BASE_TOPIC
#define B_STATE BASE "/bridge/state"
#define B_INFO  BASE "/bridge/info"
#define B_DEV   BASE "/bridge/devices"
#define B_EVT   BASE "/bridge/event"
#define BRIDGE_DEVICES_BUF_SIZE 24576u
#define BRIDGE_DEVICES_INITIAL_DELAY_MS 2000u
#define BRIDGE_DEVICES_RETRY_DELAY_MS   2000u
#define BRIDGE_DEVICES_MIN_DMA_HEAP     16384u
#define BRIDGE_PUBLIC_VERSION           "2.9.1"
#define BRIDGE_PUBLIC_COMMIT            "7419695"
#define BRIDGE_LOG_LEVEL                "info"

static bool s_bridge_devices_pending = false;
static TickType_t s_bridge_devices_due_tick = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Return friendly_name if set, else IEEE string. Result in caller's buf. */
static const char *dev_name(uint64_t ieee, const char *friendly_name,
                            char *buf, size_t buf_len)
{
    if (friendly_name && friendly_name[0]) {
        return friendly_name;
    }
    utils_ieee_to_str(ieee, buf, buf_len);
    return buf;
}

static void json_append(char **p, char *end, const char *fmt, ...)
{
    if (!p || !*p || *p >= end) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n >= end - *p) {
        *p = end;
        return;
    }
    *p += n;
}

static void json_append_string(char **p, char *end, const char *value)
{
    if (!value) value = "";

    json_append(p, end, "\"");
    while (*value && *p < end - 1) {
        unsigned char ch = (unsigned char)*value++;
        switch (ch) {
            case '\\':
                json_append(p, end, "\\\\");
                break;
            case '"':
                json_append(p, end, "\\\"");
                break;
            case '\n':
                json_append(p, end, "\\n");
                break;
            case '\r':
                json_append(p, end, "\\r");
                break;
            case '\t':
                json_append(p, end, "\\t");
                break;
            default:
                if (ch < 0x20) {
                    json_append(p, end, "\\u%04X", ch);
                } else {
                    *(*p)++ = (char)ch;
                    **p = '\0';
                }
                break;
        }
    }
    json_append(p, end, "\"");
}

static bool json_buffer_truncated(const char *p, const char *end)
{
    return p && end && p >= end;
}

static void zigbee_ieee_to_str(const esp_zb_ieee_addr_t ieee,
                               char *buf, size_t buf_len)
{
    snprintf(buf, buf_len,
             "0x%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee[7], ieee[6], ieee[5], ieee[4],
             ieee[3], ieee[2], ieee[1], ieee[0]);
}

static void append_ieee_array_json(char **p, char *end,
                                   const esp_zb_ieee_addr_t ieee)
{
    json_append(p, end, "[");
    for (int i = 0; i < 8; i++) {
        if (i) json_append(p, end, ",");
        json_append(p, end, "%u", ieee[i]);
    }
    json_append(p, end, "]");
}

static const char *z2m_interview_state(const device_record_t *dev)
{
    if (!dev) return "PENDING";

    switch (dev->state) {
        case DEV_STATE_FAILED:
            return "FAILED";
        case DEV_STATE_INTERVIEWED:
        case DEV_STATE_CONFIGURED:
            return "SUCCESSFUL";
        case DEV_STATE_INTERVIEWING:
        case DEV_STATE_NEW:
        default:
            return "PENDING";
    }
}

static const char *z2m_device_type(const device_record_t *dev)
{
    if (!dev) return "EndDevice";

    switch (dev->node_desc_flags & 0x0007u) {
        case 0x0000u:
            return "Coordinator";
        case 0x0001u:
            return "Router";
        case 0x0002u:
            return "EndDevice";
        default:
            return dev->is_sleepy ? "EndDevice" : "Router";
    }
}

static void append_cluster_list_json(char **p, char *end,
                                     const uint16_t *clusters, uint8_t count)
{
    json_append(p, end, "[");
    for (uint8_t i = 0; i < count; i++) {
        if (i) json_append(p, end, ",");
        json_append_string(p, end, utils_z2m_cluster_name(clusters[i]));
    }
    json_append(p, end, "]");
}

static void append_configured_reportings_json(char **p, char *end,
                                              const endpoint_record_t *ep)
{
    rc_configured_reporting_t reportings[16];
    size_t reporting_count = rc_get_configured_reportings_for_endpoint(
        ep, reportings, sizeof(reportings) / sizeof(reportings[0]));

    json_append(p, end, "[");
    for (size_t i = 0; i < reporting_count; i++) {
        if (i) json_append(p, end, ",");
        json_append(p, end, "{");
        json_append(p, end, "\"attribute\":");
        json_append_string(p, end, utils_z2m_attribute_name(reportings[i].cluster_id,
                                                            reportings[i].attr_id));
        json_append(p, end, ",\"cluster\":");
        json_append_string(p, end, utils_z2m_cluster_name(reportings[i].cluster_id));
        json_append(p, end, ",\"maximum_report_interval\":%u",
                    reportings[i].maximum_report_interval);
        json_append(p, end, ",\"minimum_report_interval\":%u",
                    reportings[i].minimum_report_interval);
        json_append(p, end, ",\"reportable_change\":%lu}",
                    (unsigned long)reportings[i].reportable_change);
    }
    json_append(p, end, "]");
}

static void append_bindings_json(char **p, char *end, const endpoint_record_t *ep)
{
    json_append(p, end, "[");
    if (ep) {
        for (uint8_t i = 0; i < ep->binding_count && i < MAX_BINDINGS_PER_EP; i++) {
            const binding_record_t *binding = &ep->bindings[i];
            if (i) json_append(p, end, ",");

            json_append(p, end, "{");
            json_append(p, end, "\"cluster\":");
            json_append_string(p, end, utils_z2m_cluster_name(binding->cluster_id));
            json_append(p, end, ",\"target\":{");

            if (binding->dst_addr_mode == 0x01u) {
                json_append(p, end, "\"group\":%u,\"type\":\"group\"",
                            binding->dst_group_addr);
            } else {
                char ieee_str[20];
                utils_ieee_to_str(binding->dst_ieee_addr, ieee_str, sizeof(ieee_str));
                json_append(p, end, "\"endpoint\":%u,", binding->dst_endpoint);
                json_append(p, end, "\"ieee_address\":");
                json_append_string(p, end, ieee_str);
                json_append(p, end, ",\"type\":\"endpoint\"");
            }

            json_append(p, end, "}}");
        }
    }
    json_append(p, end, "]");
}

static void append_endpoints_json(char **p, char *end, const device_record_t *dev)
{
    json_append(p, end, "\"endpoints\":{");
    bool first_ep = true;

    if (dev) {
        for (int i = 0; i < dev->endpoint_count; i++) {
            const endpoint_record_t *ep = &dev->endpoints[i];
            if (ep->endpoint_id == 0) continue;

            json_append(p, end, "%s", first_ep ? "" : ",");
            first_ep = false;

            json_append(p, end, "\"%u\":{", ep->endpoint_id);
            json_append(p, end, "\"bindings\":");
            append_bindings_json(p, end, ep);
            json_append(p, end, ",");
            json_append(p, end, "\"clusters\":{\"input\":");
            append_cluster_list_json(p, end, ep->in_clusters, ep->in_cluster_count);
            json_append(p, end, ",\"output\":");
            append_cluster_list_json(p, end, ep->out_clusters, ep->out_cluster_count);
            json_append(p, end, "},\"configured_reportings\":");
            append_configured_reportings_json(p, end, ep);
            json_append(p, end, ",\"scenes\":[]}");
        }
    }

    json_append(p, end, "}");
}

static void append_coordinator_entry_json(char **p, char *end)
{
    esp_zb_ieee_addr_t coord_ieee = {0};
    char ieee_str[20];

    esp_zb_get_long_address(coord_ieee);
    zigbee_ieee_to_str(coord_ieee, ieee_str, sizeof(ieee_str));

    json_append(p, end, "{");
    json_append(p, end, "\"disabled\":false,");
    json_append(p, end,
                "\"endpoints\":{\"1\":{\"bindings\":[],"
                "\"clusters\":{\"input\":[],\"output\":[]},"
                "\"configured_reportings\":[],\"scenes\":[]}},");
    json_append(p, end, "\"friendly_name\":\"Coordinator\",");
    json_append(p, end, "\"ieee_address\":");
    json_append_string(p, end, ieee_str);
    json_append(p, end,
                ",\"interview_completed\":true,"
                "\"interview_state\":\"SUCCESSFUL\","
                "\"interviewing\":false,"
                "\"network_address\":0,"
                "\"supported\":true,"
                "\"type\":\"Coordinator\"}");
}

static void append_device_entry_json(char **p, char *end, const device_record_t *dev)
{
    char ieee_str[20];
    char name_buf[20];
    const char *friendly_name;

    if (!dev) return;

    utils_ieee_to_str(dev->ieee_addr, ieee_str, sizeof(ieee_str));
    friendly_name = dev_name(dev->ieee_addr, dev->friendly_name,
                             name_buf, sizeof(name_buf));

    json_append(p, end, "{");
    json_append(p, end, "\"disabled\":false,");
    append_endpoints_json(p, end, dev);
    json_append(p, end, ",\"friendly_name\":");
    json_append_string(p, end, friendly_name);
    json_append(p, end, ",\"ieee_address\":");
    json_append_string(p, end, ieee_str);
    json_append(p, end, ",\"interview_completed\":%s",
                dev->state >= DEV_STATE_INTERVIEWED ? "true" : "false");
    json_append(p, end, ",\"interview_state\":");
    json_append_string(p, end, z2m_interview_state(dev));
    json_append(p, end, ",\"interviewing\":%s",
                dev->state == DEV_STATE_INTERVIEWING ? "true" : "false");
    if (dev->manufacturer[0]) {
        json_append(p, end, ",\"manufacturer\":");
        json_append_string(p, end, dev->manufacturer);
    }
    if (dev->model[0]) {
        json_append(p, end, ",\"model_id\":");
        json_append_string(p, end, dev->model);
    }
    dd_append_definition_json(p, end, dev);
    json_append(p, end, ",\"network_address\":%u", dev->nwk_addr);
    json_append(p, end, ",\"power_source\":");
    json_append_string(p, end, utils_z2m_power_source_name(dev->power_source));
    json_append(p, end, ",\"supported\":%s",
                dd_is_supported(dev) ? "true" : "false");
    json_append(p, end, ",\"type\":");
    json_append_string(p, end, z2m_device_type(dev));
    json_append(p, end, "}");
}

static char *build_bridge_devices_payload(size_t *out_len)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t buf_size = 4096;

    while (buf_size <= BRIDGE_DEVICES_BUF_SIZE) {
        char *buf = heap_caps_malloc(buf_size, caps);
        if (!buf) {
            ZB_LOG("MQTT bridge: failed to allocate %u-byte bridge/devices buffer",
                   (unsigned)buf_size);
            return NULL;
        }

        char *p = buf;
        char *end = buf + buf_size - 1;
        *p = '\0';

        json_append(&p, end, "[");
        append_coordinator_entry_json(&p, end);

        dm_lock();
        for (int i = 0; i < MAX_DEVICES; i++) {
            device_record_t *dev = dm_get_by_index(i);
            if (!dev || !dev->in_use) continue;
            if (dev->state < DEV_STATE_INTERVIEWED) continue;

            json_append(&p, end, ",");
            append_device_entry_json(&p, end, dev);
        }
        dm_unlock();

        json_append(&p, end, "]");
        if (json_buffer_truncated(p, end)) {
            free(buf);
            buf_size *= 2;
            continue;
        }

        size_t used = strlen(buf);
        char *shrunk = heap_caps_realloc(buf, used + 1, caps);
        if (shrunk) {
            buf = shrunk;
        }
        if (out_len) {
            *out_len = used;
        }
        return buf;
    }

    ZB_LOG("MQTT bridge: skip %s publish, payload exceeded %u bytes",
           B_DEV, (unsigned)BRIDGE_DEVICES_BUF_SIZE);
    return NULL;
}

static void schedule_bridge_devices_publish(TickType_t delay_ticks)
{
    s_bridge_devices_pending = true;
    s_bridge_devices_due_tick = xTaskGetTickCount() + delay_ticks;
}

static void build_bridge_info_json_fallback(char *buf, size_t buf_len)
{
    uint8_t ch = esp_zb_get_current_channel();
    uint16_t pan = esp_zb_get_pan_id();
    esp_zb_ieee_addr_t ext_pan = {0};
    char ext_pan_str[20];

    esp_zb_get_extended_pan_id(ext_pan);
    zigbee_ieee_to_str(ext_pan, ext_pan_str, sizeof(ext_pan_str));

    snprintf(buf, buf_len,
             "{\"version\":\"%s\","
             "\"network\":{\"channel\":%u,\"extended_pan_id\":\"%s\",\"pan_id\":%u},"
             "\"permit_join\":%s,"
             "\"restart_required\":false}",
             BRIDGE_PUBLIC_VERSION,
             ch,
             ext_pan_str,
             pan,
             button_handler_permit_join_active() ? "true" : "false");
}

static bool build_bridge_info_json(char *buf, size_t buf_len)
{
    uint8_t ch = esp_zb_get_current_channel();
    uint16_t pan = esp_zb_get_pan_id();
    esp_zb_ieee_addr_t ext_pan = {0};
    esp_zb_ieee_addr_t coord_ieee = {0};
    char ext_pan_str[20];
    char coord_ieee_str[20];
    char *p = buf;
    char *end = buf + buf_len - 1;

    *p = '\0';

    esp_zb_get_extended_pan_id(ext_pan);
    esp_zb_get_long_address(coord_ieee);
    zigbee_ieee_to_str(ext_pan, ext_pan_str, sizeof(ext_pan_str));
    zigbee_ieee_to_str(coord_ieee, coord_ieee_str, sizeof(coord_ieee_str));

    json_append(&p, end, "{");
    json_append(&p, end, "\"commit\":\"%s\",", BRIDGE_PUBLIC_COMMIT);

    json_append(&p, end, "\"config\":{");
    json_append(&p, end, "\"advanced\":{");
    json_append(&p, end,
                "\"cache_state\":true,"
                "\"cache_state_persistent\":true,"
                "\"cache_state_send_on_startup\":true,");
    json_append(&p, end, "\"channel\":%u,", ch);
    json_append(&p, end, "\"elapsed\":false,");
    json_append(&p, end, "\"ext_pan_id\":");
    append_ieee_array_json(&p, end, ext_pan);
    json_append(&p, end, ",\"last_seen\":\"disable\",");
    json_append(&p, end, "\"log_level\":\"%s\",", BRIDGE_LOG_LEVEL);
    json_append(&p, end, "\"output\":\"json\",");
    json_append(&p, end, "\"pan_id\":%u", pan);
    json_append(&p, end, "},");

    json_append(&p, end,
                "\"device_options\":{},"
                "\"frontend\":{\"enabled\":false},"
                "\"groups\":{},"
                "\"health\":{\"interval\":30,\"reset_on_check\":false},"
                "\"homeassistant\":{\"enabled\":false},");

    json_append(&p, end, "\"mqtt\":{");
    json_append(&p, end, "\"base_topic\":\"%s\",", MQTT_BASE_TOPIC);
    json_append(&p, end,
                "\"include_device_information\":false,"
                "\"keepalive\":60,"
                "\"reject_unauthorized\":true,");
    json_append(&p, end, "\"server\":\"%s\",", MQTT_BROKER_URI);
    json_append(&p, end, "\"version\":4},");

    json_append(&p, end, "\"serial\":{");
    json_append(&p, end, "\"adapter\":\"zboss\",");
    json_append(&p, end, "\"port\":\"%s\"},", CONFIG_IDF_TARGET);
    json_append(&p, end, "\"version\":5");
    json_append(&p, end, "},");

    json_append(&p, end, "\"coordinator\":{");
    json_append(&p, end, "\"ieee_address\":");
    json_append_string(&p, end, coord_ieee_str);
    json_append(&p, end, ",\"meta\":{},\"type\":\"ZBOSS\"},");

    json_append(&p, end, "\"log_level\":\"%s\",", BRIDGE_LOG_LEVEL);
    json_append(&p, end,
                "\"mqtt\":{\"server\":\"%s\",\"version\":4},",
                MQTT_BROKER_URI);
    json_append(&p, end,
                "\"network\":{\"channel\":%u,\"extended_pan_id\":",
                ch);
    json_append_string(&p, end, ext_pan_str);
    json_append(&p, end, ",\"pan_id\":%u},", pan);
    json_append(&p, end, "\"permit_join\":%s,",
                button_handler_permit_join_active() ? "true" : "false");
    json_append(&p, end, "\"restart_required\":false,");
    json_append(&p, end, "\"version\":\"%s\"", BRIDGE_PUBLIC_VERSION);
    json_append(&p, end, "}");

    return !json_buffer_truncated(p, end);
}

/** Publish directly via MQTT client (for use in mqtt_bridge_on_connected). */
static void direct_pub(const char *topic, const char *payload,
                       int qos, int retain)
{
    esp_mqtt_client_handle_t c = mqtt_manager_get_client();
    if (!c) return;
    ZB_LOG("MQTT TX topic=%s payload=%s", topic, payload);
    esp_mqtt_client_publish(c, topic, payload, (int)strlen(payload), qos, retain);
}

static void burst_pub(const char *topic, const char *payload,
                      int qos, int retain, TickType_t settle_ticks)
{
    direct_pub(topic, payload, qos, retain);
    if (settle_ticks > 0) {
        vTaskDelay(settle_ticks);
    } else {
        taskYIELD();
    }
}

/** Enqueue a publish (for use from Zigbee task context). */
static void enqueue_pub(const char *topic, const char *payload,
                        uint8_t qos, bool retain)
{
    mqtt_manager_publish(topic, payload, qos, retain);
}

static void pub_bridge_info(bool direct)
{
    char info[MQTT_MAX_PAYLOAD_LEN];

    if (!build_bridge_info_json(info, sizeof(info))) {
        ZB_LOG("MQTT bridge: %s payload truncated, using fallback payload", B_INFO);
        build_bridge_info_json_fallback(info, sizeof(info));
    }

    if (direct) {
        burst_pub(B_INFO, info, 1, 1, pdMS_TO_TICKS(20));
    } else {
        enqueue_pub(B_INFO, info, 1, true);
    }
}

// ---------------------------------------------------------------------------
// Build JSON payload for a single device's ZCL state
// (all known attributes from cache + linkquality + last_seen)
// ---------------------------------------------------------------------------

static void build_device_state_json(uint64_t ieee, uint8_t lqi,
                                    bool has_lqi,
                                    uint32_t last_seen_ms,
                                    char *buf, size_t buf_len)
{
    char *p   = buf;
    char *end = buf + buf_len - 2;   // leave room for "}"

    p += snprintf(p, end - p, "{");
    bool first = true;
    if (has_lqi) {
        p += snprintf(p, end - p, "\"linkquality\":%u", lqi);
        first = false;
    }
    p += snprintf(p, end - p, "%s\"last_seen\":%.3f",
                  first ? "" : ",",
                  (double)last_seen_ms / 1000.0);

    first = false;
    zcl_fill_state_json(ieee, p, (size_t)(end - p), &first);

    // Advance p to end of string
    p += strlen(p);
    if (p < end) {
        *p++ = '}';
        *p   = '\0';
    }
}

// ---------------------------------------------------------------------------
// Publish helpers (internal)
// ---------------------------------------------------------------------------

/** Publish device state - enqueue version (from Zigbee task). */
static void pub_device_state_enqueue(const zb_event_t *evt)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char ibuf[20];
    const char *name = dev_name(evt->ieee, evt->friendly_name, ibuf, sizeof(ibuf));
    snprintf(topic, sizeof(topic), BASE "/%s", name);

    char payload[MQTT_MAX_PAYLOAD_LEN];
    build_device_state_json(evt->ieee, evt->lqi, evt->has_lqi,
                            (uint32_t)(utils_uptime_ms()),
                            payload, sizeof(payload));
    enqueue_pub(topic, payload, 0, true);
}

/** Publish device state - direct version (from mqtt_task reconnect burst). */
static void pub_device_state_direct(const device_record_t *dev)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char ibuf[20];
    const char *name = dev_name(dev->ieee_addr, dev->friendly_name, ibuf, sizeof(ibuf));
    snprintf(topic, sizeof(topic), BASE "/%s", name);

    char payload[MQTT_MAX_PAYLOAD_LEN];
    build_device_state_json(dev->ieee_addr, dev->last_lqi, dev->radio_metrics_valid,
                            dev->last_seen_ms,
                            payload, sizeof(payload));
    direct_pub(topic, payload, 0, 1);
}

static void pub_availability(uint64_t ieee, const char *friendly_name,
                             bool online, bool direct)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char ibuf[20];
    const char *name = dev_name(ieee, friendly_name, ibuf, sizeof(ibuf));
    snprintf(topic, sizeof(topic), BASE "/%s/availability", name);

    const char *payload = online ? "{\"state\":\"online\"}"
                                 : "{\"state\":\"offline\"}";
    if (direct) {
        direct_pub(topic, payload, 1, 1);
    } else {
        enqueue_pub(topic, payload, 1, true);
    }
}

// ---------------------------------------------------------------------------
// zb_events handler - called synchronously from Zigbee task
// ---------------------------------------------------------------------------

static void on_zigbee_event(const zb_event_t *evt)
{
    char ibuf[20];
    char payload[512];

    switch (evt->type) {

        case ZB_EVT_DEVICE_JOINED:
        {
            utils_ieee_to_str(evt->ieee, ibuf, sizeof(ibuf));
            const char *name = dev_name(evt->ieee, evt->friendly_name,
                                        ibuf, sizeof(ibuf));
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"device_joined\","
                     "\"data\":{\"friendly_name\":\"%s\","
                     "\"ieee_address\":\"%s\"}}",
                     name, ibuf);
            enqueue_pub(B_EVT, payload, 0, false);
            mqtt_bridge_request_devices_refresh();
            break;
        }

        case ZB_EVT_DEVICE_LEAVE:
        {
            utils_ieee_to_str(evt->ieee, ibuf, sizeof(ibuf));
            const char *name = dev_name(evt->ieee, evt->friendly_name,
                                        ibuf, sizeof(ibuf));
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"device_leave\","
                     "\"data\":{\"friendly_name\":\"%s\","
                     "\"ieee_address\":\"%s\"}}",
                     name, ibuf);
            enqueue_pub(B_EVT, payload, 0, false);
            mqtt_bridge_request_devices_refresh();
            break;
        }

        case ZB_EVT_INTERVIEW:
        {
            utils_ieee_to_str(evt->ieee, ibuf, sizeof(ibuf));
            const char *name = dev_name(evt->ieee, evt->friendly_name,
                                        ibuf, sizeof(ibuf));
            const char *status = evt->interview_status ? evt->interview_status : "unknown";
            bool successful = (strcmp(status, "successful") == 0);

            if (successful) {
                snprintf(payload, sizeof(payload),
                         "{\"type\":\"device_interview\","
                         "\"data\":{\"friendly_name\":\"%s\","
                         "\"ieee_address\":\"%s\","
                         "\"status\":\"successful\",\"supported\":true}}",
                         name, ibuf);
            } else {
                snprintf(payload, sizeof(payload),
                         "{\"type\":\"device_interview\","
                         "\"data\":{\"friendly_name\":\"%s\","
                         "\"ieee_address\":\"%s\","
                         "\"status\":\"%s\"}}",
                         name, ibuf, status);
            }
            enqueue_pub(B_EVT, payload, 0, false);
            if (successful) {
                mqtt_bridge_request_devices_refresh();
            }
            break;
        }

        case ZB_EVT_ATTR_CHANGED:
            // Build and enqueue full device state JSON
            pub_device_state_enqueue(evt);
            break;

        case ZB_EVT_AVAILABILITY:
            pub_availability(evt->ieee, evt->friendly_name, evt->online, false);
            break;

        case ZB_EVT_PERMIT_JOIN:
        {
            // Publish a bridge/event for permit-join status
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"permit_join\","
                     "\"data\":{\"value\":%s,\"time\":%u}}",
                     evt->permit_join_duration > 0 ? "true" : "false",
                     evt->permit_join_duration);
            enqueue_pub(B_EVT, payload, 0, false);
            pub_bridge_info(false);
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// mqtt_bridge_on_connected - called from mqtt_task, direct publishes
// ---------------------------------------------------------------------------

void mqtt_bridge_on_connected(void)
{
    ZB_LOG("MQTT bridge: publishing initial state burst");

    // 1. Bridge online
    burst_pub(B_STATE, "{\"state\":\"online\"}", 1, 1, pdMS_TO_TICKS(20));

    // 2. Bridge info
    pub_bridge_info(true);

    // 3. All device states and availability
    {
        dm_lock();
        for (int i = 0; i < MAX_DEVICES; i++) {
            device_record_t *dev = dm_get_by_index(i);
            if (!dev || !dev->in_use) continue;
            if (dev->state < DEV_STATE_INTERVIEWED) continue;

            pub_device_state_direct(dev);
            vTaskDelay(pdMS_TO_TICKS(15));
            pub_availability(dev->ieee_addr, dev->friendly_name, dev->online, true);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        dm_unlock();
    }

    schedule_bridge_devices_publish(pdMS_TO_TICKS(BRIDGE_DEVICES_INITIAL_DELAY_MS));
    ZB_LOG("MQTT bridge: deferred %s publication scheduled", B_DEV);
    ZB_LOG("MQTT bridge: initial burst complete");
}

void mqtt_bridge_poll(void)
{
    if (!s_bridge_devices_pending) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_bridge_devices_due_tick) < 0) {
        return;
    }

    esp_mqtt_client_handle_t client = mqtt_manager_get_client();
    if (!client) {
        schedule_bridge_devices_publish(pdMS_TO_TICKS(BRIDGE_DEVICES_RETRY_DELAY_MS));
        return;
    }

    int outbox_size = esp_mqtt_client_get_outbox_size(client);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (outbox_size > 0 || free_dma < BRIDGE_DEVICES_MIN_DMA_HEAP) {
        ZB_LOG("MQTT bridge: defer %s (outbox=%d, free DMA-capable heap=%u bytes)",
               B_DEV, outbox_size, (unsigned)free_dma);
        schedule_bridge_devices_publish(pdMS_TO_TICKS(BRIDGE_DEVICES_RETRY_DELAY_MS));
        return;
    }

    size_t devices_len = 0;
    char *devices_buf = build_bridge_devices_payload(&devices_len);
    if (!devices_buf) {
        ZB_LOG("MQTT bridge: defer %s (payload build failed)", B_DEV);
        schedule_bridge_devices_publish(pdMS_TO_TICKS(BRIDGE_DEVICES_RETRY_DELAY_MS));
        return;
    }

    free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (free_dma < BRIDGE_DEVICES_MIN_DMA_HEAP) {
        ZB_LOG("MQTT bridge: defer %s after build (payload=%u bytes, free DMA-capable heap=%u bytes)",
               B_DEV, (unsigned)devices_len, (unsigned)free_dma);
        free(devices_buf);
        schedule_bridge_devices_publish(pdMS_TO_TICKS(BRIDGE_DEVICES_RETRY_DELAY_MS));
        return;
    }

    ZB_LOG("MQTT bridge: publishing deferred %s payload=%u bytes, free DMA-capable heap=%u bytes",
           B_DEV, (unsigned)devices_len, (unsigned)free_dma);
    burst_pub(B_DEV, devices_buf, 1, 1, pdMS_TO_TICKS(20));
    free(devices_buf);
    s_bridge_devices_pending = false;
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

void mqtt_bridge_init(void)
{
    zb_events_register(on_zigbee_event);
    ZB_LOG("MQTT bridge: registered with event bus");
}

void mqtt_bridge_request_devices_refresh(void)
{
    schedule_bridge_devices_publish(pdMS_TO_TICKS(200));
}

void mqtt_bridge_republish_device_after_rename(uint64_t ieee,
                                               const char *old_topic_name,
                                               const char *new_topic_name)
{
    char topic[MQTT_MAX_TOPIC_LEN];

    if (old_topic_name && new_topic_name &&
        strcmp(old_topic_name, new_topic_name) != 0) {
        snprintf(topic, sizeof(topic), BASE "/%s", old_topic_name);
        enqueue_pub(topic, "", 0, true);

        snprintf(topic, sizeof(topic), BASE "/%s/availability", old_topic_name);
        enqueue_pub(topic, "", 1, true);
    }

    device_record_t *dev = dm_find_by_ieee(ieee);
    if (dev && new_topic_name && new_topic_name[0] != '\0') {
        zb_event_t evt = {
            .type    = ZB_EVT_ATTR_CHANGED,
            .ieee    = dev->ieee_addr,
            .lqi     = dev->last_lqi,
            .has_lqi = dev->radio_metrics_valid,
        };
        strncpy(evt.friendly_name, new_topic_name, ZB_EVT_NAME_LEN - 1);
        evt.friendly_name[ZB_EVT_NAME_LEN - 1] = '\0';
        pub_device_state_enqueue(&evt);
        pub_availability(dev->ieee_addr, new_topic_name, dev->online, false);
    }

    mqtt_bridge_request_devices_refresh();
}
