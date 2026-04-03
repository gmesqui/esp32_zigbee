#include "mqtt_bridge.h"
#include "zb_events.h"
#include "mqtt_manager.h"
#include "device_manager.h"
#include "zcl_handler.h"
#include "utils.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_zigbee_core.h"   // esp_zb_get_current_channel, esp_zb_get_pan_id

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define BASE    "esp32_zigbee"
#define B_STATE BASE "/bridge/state"
#define B_INFO  BASE "/bridge/info"
#define B_DEV   BASE "/bridge/devices"
#define B_EVT   BASE "/bridge/event"

// Static buffer for bridge/devices payload (up to 32 devices × ~200 bytes)
static char s_devices_buf[7168];

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

/** Publish directly via MQTT client (for use in mqtt_bridge_on_connected). */
static void direct_pub(const char *topic, const char *payload,
                        int qos, int retain)
{
    esp_mqtt_client_handle_t c = mqtt_manager_get_client();
    if (!c) return;
    esp_mqtt_client_publish(c, topic, payload, (int)strlen(payload), qos, retain);
}

/** Enqueue a publish (for use from Zigbee task context). */
static void enqueue_pub(const char *topic, const char *payload,
                         uint8_t qos, bool retain)
{
    mqtt_manager_publish(topic, payload, qos, retain);
}

// ---------------------------------------------------------------------------
// Build JSON payload for a single device's ZCL state
// (all known attributes from cache + linkquality + last_seen)
// ---------------------------------------------------------------------------

static void build_device_state_json(uint64_t ieee, uint8_t lqi,
                                     uint32_t last_seen_ms,
                                     char *buf, size_t buf_len)
{
    char *p   = buf;
    char *end = buf + buf_len - 2;   // leave room for "}"

    p += snprintf(p, end - p,
                  "{\"linkquality\":%u,\"last_seen\":%.3f",
                  lqi, (double)last_seen_ms / 1000.0);

    bool first = false;   // comma already written above
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

/** Publish device state — enqueue version (from Zigbee task). */
static void pub_device_state_enqueue(const zb_event_t *evt)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char ibuf[20];
    const char *name = dev_name(evt->ieee, evt->friendly_name, ibuf, sizeof(ibuf));
    snprintf(topic, sizeof(topic), BASE "/%s", name);

    char payload[MQTT_MAX_PAYLOAD_LEN];
    build_device_state_json(evt->ieee, evt->lqi,
                             (uint32_t)(utils_uptime_ms()),
                             payload, sizeof(payload));
    enqueue_pub(topic, payload, 0, true);
}

/** Publish device state — direct version (from mqtt_task reconnect burst). */
static void pub_device_state_direct(const device_record_t *dev)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char ibuf[20];
    const char *name = dev_name(dev->ieee_addr, dev->friendly_name, ibuf, sizeof(ibuf));
    snprintf(topic, sizeof(topic), BASE "/%s", name);

    char payload[MQTT_MAX_PAYLOAD_LEN];
    build_device_state_json(dev->ieee_addr, dev->last_lqi, dev->last_seen_ms,
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
// zb_events handler — called synchronously from Zigbee task
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
            pub_availability(evt->ieee, evt->friendly_name, false, false);
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
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// mqtt_bridge_on_connected — called from mqtt_task, direct publishes
// ---------------------------------------------------------------------------

void mqtt_bridge_on_connected(void)
{
    ZB_LOG("MQTT bridge: publishing initial state burst");

    // 1. Bridge online
    direct_pub(B_STATE, "{\"state\":\"online\"}", 1, 1);

    // 2. Bridge info
    {
        char info[512];
        uint8_t ch  = esp_zb_get_current_channel();
        uint16_t pan = esp_zb_get_pan_id();
        snprintf(info, sizeof(info),
                 "{\"version\":\"1.0.0\","
                 "\"network\":{\"channel\":%u,\"pan_id\":\"0x%04X\"},"
                 "\"permit_join\":false}",
                 ch, pan);
        direct_pub(B_INFO, info, 1, 1);
    }

    // 3. All device states and availability
    {
        char *dp = s_devices_buf;
        char *dend = s_devices_buf + sizeof(s_devices_buf) - 2;
        dp += snprintf(dp, (size_t)(dend - dp), "[");
        bool first_dev = true;

        dm_lock();
        for (int i = 0; i < MAX_DEVICES; i++) {
            device_record_t *dev = dm_get_by_index(i);
            if (!dev || !dev->in_use) continue;
            if (dev->state < DEV_STATE_INTERVIEWED) continue;

            // Publish individual state and availability
            pub_device_state_direct(dev);
            pub_availability(dev->ieee_addr, dev->friendly_name, dev->online, true);

            // Append to bridge/devices array
            if (dp < dend) {
                char ibuf[20];
                utils_ieee_to_str(dev->ieee_addr, ibuf, sizeof(ibuf));
                const char *name = dev->friendly_name[0]
                                    ? dev->friendly_name : ibuf;
                dp += snprintf(dp, (size_t)(dend - dp),
                               "%s{"
                               "\"ieee_address\":\"%s\","
                               "\"friendly_name\":\"%s\","
                               "\"manufacturer\":\"%s\","
                               "\"model\":\"%s\","
                               "\"power_source\":\"%s\","
                               "\"interview_completed\":%s,"
                               "\"type\":\"%s\""
                               "}",
                               first_dev ? "" : ",",
                               ibuf, name,
                               dev->manufacturer,
                               dev->model,
                               utils_power_source_name(dev->power_source),
                               (dev->state >= DEV_STATE_INTERVIEWED) ? "true" : "false",
                               dev->is_sleepy ? "EndDevice" : "Router");
                first_dev = false;
            }
        }
        dm_unlock();

        if (dp < dend) {
            *dp++ = ']';
            *dp   = '\0';
        }
        direct_pub(B_DEV, s_devices_buf, 1, 1);
    }

    ZB_LOG("MQTT bridge: initial burst complete");
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

void mqtt_bridge_init(void)
{
    zb_events_register(on_zigbee_event);
    ZB_LOG("MQTT bridge: registered with event bus");
}
