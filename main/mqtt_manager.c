#include "mqtt_manager.h"
#include "mqtt_bridge.h"
#include "mqtt_commands.h"
#include "eth_driver.h"
#include "utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <inttypes.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define MQTT_CLIENT_ID      "esp32_zigbee"
#define MQTT_LWT_TOPIC      MQTT_BASE_TOPIC "/bridge/state"
#define MQTT_LWT_PAYLOAD    "{\"state\":\"offline\"}"
#define MQTT_KEEPALIVE_S    60

#define BACKOFF_MAX_S       30
#define MQTT_TASK_STACK     8192
#define MQTT_CLIENT_STACK   4096
#define MQTT_TASK_PRIORITY  4

// Task notification bits
#define NOTIFY_CONNECTED    BIT0
#define NOTIFY_DISCONNECTED BIT1

typedef struct {
    char topic[MQTT_MAX_TOPIC_LEN];
    char payload[MQTT_MAX_PAYLOAD_LEN];
} mqtt_rx_msg_t;

#define MQTT_RX_QUEUE_LEN 8

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static QueueHandle_t              s_queue;
static QueueHandle_t              s_rx_queue;
static EventGroupHandle_t         s_eth_eg;
static esp_mqtt_client_handle_t   s_client;
static TaskHandle_t               s_task_handle;
static volatile bool              s_connected = false;

static void subscribe_command_topics(esp_mqtt_client_handle_t client)
{
    if (!client) {
        return;
    }

    esp_mqtt_client_subscribe(client, MQTT_BASE_TOPIC "/bridge/request/#", 1);
    esp_mqtt_client_subscribe(client, MQTT_BASE_TOPIC "/+/set/#", 1);
    esp_mqtt_client_subscribe(client, MQTT_BASE_TOPIC "/+/get/#", 1);
}

// ---------------------------------------------------------------------------
// MQTT event handler (runs in esp_mqtt internal task)
// ---------------------------------------------------------------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    mqtt_rx_msg_t rx_msg;
    (void)handler_args;
    (void)base;
    (void)event_id;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ZB_LOG("MQTT: connected to broker");
            s_connected = true;
            subscribe_command_topics(s_client);
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

        case MQTT_EVENT_DATA:
            if (!event->topic || event->topic_len <= 0) {
                break;
            }
            if (event->current_data_offset != 0 ||
                event->data_len != event->total_data_len) {
                ZB_LOG("MQTT RX fragmented payload not supported topic_len=%d total=%d",
                       event->topic_len, event->total_data_len);
                break;
            }
            if (event->topic_len >= (int)sizeof(rx_msg.topic) ||
                event->data_len >= (int)sizeof(rx_msg.payload)) {
                ZB_LOG("MQTT RX drop oversized message topic_len=%d payload_len=%d",
                       event->topic_len, event->data_len);
                break;
            }

            memset(&rx_msg, 0, sizeof(rx_msg));
            memcpy(rx_msg.topic, event->topic, (size_t)event->topic_len);
            memcpy(rx_msg.payload, event->data, (size_t)event->data_len);

            if (xQueueSend(s_rx_queue, &rx_msg, 0) != pdTRUE) {
                ZB_LOG("MQTT RX queue full, drop topic=%s", rx_msg.topic);
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Queue drainers
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

static void drain_rx_queue(void)
{
    mqtt_rx_msg_t msg;

    while (s_connected && xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
        ZB_LOG("MQTT RX topic=%s payload=%s", msg.topic, msg.payload);
        mqtt_commands_on_message(msg.topic, msg.payload);
    }
}

// ---------------------------------------------------------------------------
// MQTT task
// ---------------------------------------------------------------------------
static void mqtt_task(void *arg)
{
    uint32_t backoff_attempt = 0;
    uint32_t notify_val;

    (void)arg;

    ZB_LOG("MQTT task: waiting for Ethernet IP");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ZB_LOG("MQTT task: IP ready, connecting to broker");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will.topic   = MQTT_LWT_TOPIC,
        .session.last_will.msg     = MQTT_LWT_PAYLOAD,
        .session.last_will.msg_len = (int)strlen(MQTT_LWT_PAYLOAD),
        .session.last_will.qos     = 1,
        .session.last_will.retain  = 1,
        .network.reconnect_timeout_ms = 0,
        .task.priority = MQTT_TASK_PRIORITY,
        .task.stack_size = MQTT_CLIENT_STACK,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    for (;;) {
        if (xTaskNotifyWait(0, NOTIFY_CONNECTED | NOTIFY_DISCONNECTED,
                            &notify_val, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (notify_val & NOTIFY_CONNECTED) {
                backoff_attempt = 0;
                mqtt_bridge_on_connected();
                drain_rx_queue();
                drain_queue();
            }

            if (notify_val & NOTIFY_DISCONNECTED) {
                uint32_t delay_s = (1u << backoff_attempt);
                if (delay_s > BACKOFF_MAX_S) {
                    delay_s = BACKOFF_MAX_S;
                }
                if (backoff_attempt < 31) {
                    backoff_attempt++;
                }

                ZB_LOG("MQTT: reconnecting in %"PRIu32" s (attempt %"PRIu32")",
                       delay_s, backoff_attempt);
                vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));

                xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                                    pdFALSE, pdTRUE, portMAX_DELAY);
                esp_mqtt_client_reconnect(s_client);
            }
        } else if (s_connected) {
            drain_rx_queue();
            drain_queue();
            mqtt_bridge_poll();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void mqtt_manager_init(EventGroupHandle_t eth_ready_eg)
{
    s_eth_eg = eth_ready_eg;
    s_queue = xQueueCreate(MQTT_QUEUE_LEN, sizeof(mqtt_msg_t));
    s_rx_queue = xQueueCreate(MQTT_RX_QUEUE_LEN, sizeof(mqtt_rx_msg_t));
    configASSERT(s_queue);
    configASSERT(s_rx_queue);

    xTaskCreate(mqtt_task, "mqtt_task", MQTT_TASK_STACK, NULL,
                MQTT_TASK_PRIORITY, &s_task_handle);
}

bool mqtt_manager_publish(const char *topic, const char *payload,
                          uint8_t qos, bool retain)
{
    mqtt_msg_t msg;

    if (!topic || !payload) return false;
    if (strlen(topic) >= MQTT_MAX_TOPIC_LEN) {
        ZB_LOG("MQTT: topic too long, drop publish");
        return false;
    }
    if (strlen(payload) >= MQTT_MAX_PAYLOAD_LEN) {
        ZB_LOG("MQTT: payload too long for queue, drop topic=%s", topic);
        return false;
    }

    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.topic[sizeof(msg.topic) - 1] = '\0';
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    msg.qos = qos;
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
