#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/** Starts SNTP time synchronization once Ethernet has an IP address. */
void time_sync_init(EventGroupHandle_t eth_ready_eg);
