#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

// ---------------------------------------------------------------------------
// MQTT manager — connection management, publish queue and reconnect logic.
//
// Any task can call mqtt_manager_publish() safely (non-blocking).
// The mqtt_task (priority 4) drains the queue and owns the MQTT client.
// ---------------------------------------------------------------------------

#define MQTT_MAX_TOPIC_LEN    128
#define MQTT_MAX_PAYLOAD_LEN  768
#define MQTT_QUEUE_LEN        16

typedef struct {
    char    topic[MQTT_MAX_TOPIC_LEN];
    char    payload[MQTT_MAX_PAYLOAD_LEN];
    uint8_t qos;
    bool    retain;
} mqtt_msg_t;

/**
 * Initialise MQTT manager.  eth_ready_eg must be the event group returned by
 * eth_driver_init(); ETH_IP_READY_BIT will be waited on before connecting.
 * Starts the mqtt_task.
 */
void mqtt_manager_init(EventGroupHandle_t eth_ready_eg);

/**
 * Enqueue a publish message.  Non-blocking (0-tick timeout).
 * Safe to call from any task, including the Zigbee task.
 * Returns false if the queue is full (message dropped).
 */
bool mqtt_manager_publish(const char *topic, const char *payload,
                           uint8_t qos, bool retain);

/** True if currently connected to the MQTT broker. */
bool mqtt_manager_is_connected(void);

/**
 * Return the underlying esp_mqtt_client handle.
 * Used by mqtt_bridge_on_connected() for direct synchronous publishes
 * during the reconnect burst (must only be called from mqtt_task context).
 */
esp_mqtt_client_handle_t mqtt_manager_get_client(void);
