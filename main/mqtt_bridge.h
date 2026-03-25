#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_bridge_init(void);
void mqtt_bridge_poll(void);
esp_err_t mqtt_bridge_notify_zigbee_event(const char *event_name);

#ifdef __cplusplus
}
#endif
