#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool started;
    bool synced;
    char server[64];
    char timezone[48];
    uint32_t last_sync_uptime_s;
} time_sync_status_t;

/** Starts SNTP time synchronization once Ethernet has an IP address. */
void time_sync_init(EventGroupHandle_t eth_ready_eg);

/** Restart SNTP using the current app_config values. */
void time_sync_restart(void);

void time_sync_get_status(time_sync_status_t *out);
