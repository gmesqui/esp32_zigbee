#pragma once

// ---------------------------------------------------------------------------
// MQTT command router.
//
// Consumes incoming MQTT messages (already copied into NUL-terminated buffers)
// and executes supported zigbee2mqtt-compatible requests.
// ---------------------------------------------------------------------------

/** Route and execute one incoming MQTT message. */
void mqtt_commands_on_message(const char *topic, const char *payload);
