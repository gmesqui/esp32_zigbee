#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// MQTT bridge — translates Zigbee events to zigbee2mqtt-compatible MQTT msgs.
//
// Consumes events from the zb_events bus (registered in mqtt_bridge_init).
// Does NOT expose any Zigbee-specific types in its public API.
//
// Base topic: esp32_zigbee
// Broker:     orangepipcplus.local (resolved via mDNS)
// ---------------------------------------------------------------------------

/**
 * Initialise the MQTT bridge.
 * Registers this module as a zb_events consumer.
 * Must be called after mqtt_manager_init() and zb_events_init().
 */
void mqtt_bridge_init(void);

/**
 * Called by mqtt_manager when an MQTT connection is established.
 * Publishes: bridge/state=online, bridge/info,
 *            full state + availability for every known device.
 * Schedules bridge/devices for deferred publication once the MQTT outbox
 * drains and DMA-capable heap is available.
 * Uses esp_mqtt_client_publish() directly (synchronous burst, no queue).
 */
void mqtt_bridge_on_connected(void);

/**
 * Called periodically from mqtt_task while connected.
 * Publishes deferred bridge/devices when transport conditions allow it.
 */
void mqtt_bridge_poll(void);

/** Schedule a retained bridge/devices refresh. */
void mqtt_bridge_request_devices_refresh(void);

/**
 * Clear retained topics under the old device name and republish retained state
 * under the new one, then schedule a bridge/devices refresh.
 */
void mqtt_bridge_republish_device_after_rename(uint64_t ieee,
                                               const char *old_topic_name,
                                               const char *new_topic_name);
