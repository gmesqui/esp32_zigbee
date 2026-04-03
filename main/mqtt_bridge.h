#pragma once

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
 * Publishes: bridge/state=online, bridge/info, bridge/devices,
 *            full state + availability for every known device.
 * Uses esp_mqtt_client_publish() directly (synchronous burst, no queue).
 */
void mqtt_bridge_on_connected(void);
