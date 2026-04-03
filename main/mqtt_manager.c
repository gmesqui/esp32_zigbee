#include "mqtt_manager.h"
#include "mqtt_bridge.h"
#include "eth_driver.h"
#include "utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define MQTT_BROKER_URI     "mqtt://orangepipcplus.local"
#define MQTT_CLIENT_ID      "esp32_zigbee"
#define MQTT_BASE_TOPIC     "esp32_zigbee"
#define MQTT_LWT_TOPIC      MQTT_BASE_TOPIC "/bridge/state"
#define MQTT_LWT_PAYLOAD    "{\"state\":\"offline\"}"
#define MQTT_KEEPALIVE_S    60

#define BACKOFF_MAX_S       30
#define MQTT_TASK_STACK     8192
#define MQTT_TASK_PRIORITY  4

// Task notification bits
#define NOTIFY_CONNECTED    BIT0
#define NOTIFY_DISCONNECTED BIT1

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static QueueHandle_t              s_queue;
static EventGroupHandle_t         s_eth_eg;
static esp_mqtt_client_handle_t   s_client;
static TaskHandle_t               s_task_handle;
static volatile bool              s_connected = false;

// ---------------------------------------------------------------------------
// MQTT event handler (runs in esp_mqtt internal task)
// ---------------------------------------------------------------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    (void)handler_args;
    (void)base;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ZB_LOG("MQTT: connected to broker");
            s_connected = true;
            xTaskNotify(s_task_handle, NOTIFY_CONNECTED, eSetBits);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ZB_LOG("MQTT: disconnected");
            s_connected = false;
            xTaskNotify(s_task_handle, NOTIFY_DISCONNECTED, eSetBits);
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle) {
                ZB_LOG("MQTT: error type=%d errno=%d",
                       event->error_handle->error_type,
                       event->error_handle->esp_transport_sock_errno);
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Drain publish queue (used in CONNECTED state)
// ---------------------------------------------------------------------------
static void drain_queue(void)
{
    mqtt_msg_t msg;
    while (s_connected &&
           xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
        ZB_LOG("MQTT TX topic=%s payload=%s", msg.topic, msg.payload);
        esp_mqtt_client_publish(s_client, msg.topic, msg.payload,
                                 (int)strlen(msg.payload),
                                 msg.qos, msg.retain ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// MQTT task
// ---------------------------------------------------------------------------
static void mqtt_task(void *arg)
{
    uint32_t backoff_attempt = 0;
    uint32_t notify_val;

    // --- Wait for Ethernet / IP ---
    ZB_LOG("MQTT task: waiting for Ethernet IP");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ZB_LOG("MQTT task: IP ready, connecting to broker");

    // --- Configure and start MQTT client ---
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will.topic   = MQTT_LWT_TOPIC,
        .session.last_will.msg     = MQTT_LWT_PAYLOAD,
        .session.last_will.msg_len = (int)strlen(MQTT_LWT_PAYLOAD),
        .session.last_will.qos     = 1,
        .session.last_will.retain  = 1,
        // Disable internal auto-reconnect; we manage reconnect manually
        .network.reconnect_timeout_ms = 0,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    // --- Main loop ---
    for (;;) {
        // Wait for CONNECTED or DISCONNECTED notification (100 ms timeout
        // so we keep draining the queue while connected)
        if (xTaskNotifyWait(0, NOTIFY_CONNECTED | NOTIFY_DISCONNECTED,
                             &notify_val, pdMS_TO_TICKS(100)) == pdTRUE) {

            if (notify_val & NOTIFY_CONNECTED) {
                backoff_attempt = 0;
                // Run the reconnect burst (synchronous, direct publishes)
                mqtt_bridge_on_connected();
                // Now drain anything that queued up during the burst
                drain_queue();
            }

            if (notify_val & NOTIFY_DISCONNECTED) {
                // Exponential backoff before reconnecting
                uint32_t delay_s = (1u << backoff_attempt);
                if (delay_s > BACKOFF_MAX_S) delay_s = BACKOFF_MAX_S;
                if (backoff_attempt < 31) backoff_attempt++;

                ZB_LOG("MQTT: reconnecting in %"PRIu32" s (attempt %"PRIu32")",
                       delay_s, backoff_attempt);
                vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));

                // Check IP is still available before reconnecting
                xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                                    pdFALSE, pdTRUE, portMAX_DELAY);
                esp_mqtt_client_reconnect(s_client);
            }
        } else {
            // Timeout — keep draining if connected
            if (s_connected) {
                drain_queue();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void mqtt_manager_init(EventGroupHandle_t eth_ready_eg)
{
    s_eth_eg = eth_ready_eg;
    s_queue  = xQueueCreate(MQTT_QUEUE_LEN, sizeof(mqtt_msg_t));
    configASSERT(s_queue);

    xTaskCreate(mqtt_task, "mqtt_task", MQTT_TASK_STACK, NULL,
                MQTT_TASK_PRIORITY, &s_task_handle);
}

bool mqtt_manager_publish(const char *topic, const char *payload,
                           uint8_t qos, bool retain)
{
    if (!topic || !payload) return false;

    mqtt_msg_t msg;
    strncpy(msg.topic,   topic,   sizeof(msg.topic)   - 1);
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.topic[sizeof(msg.topic)     - 1] = '\0';
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    msg.qos    = qos;
    msg.retain = retain;

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ZB_LOG("MQTT: queue full, drop topic=%s", topic);
        return false;
    }
    return true;
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}

esp_mqtt_client_handle_t mqtt_manager_get_client(void)
{
    return s_client;
}
