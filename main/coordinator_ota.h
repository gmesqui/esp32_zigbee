#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Called after NVS is available and before starting Zigbee.
void coordinator_ota_init(void);
bool coordinator_ota_trial_boot(void);
// Called from the Zigbee DEVICE_REBOOT callback, with stack access.
bool coordinator_ota_network_matches(void);
// Called after the normal WebSocket service is listening on Ethernet.
esp_err_t coordinator_ota_start_server(void);
