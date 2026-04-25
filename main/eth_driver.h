#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

// ---------------------------------------------------------------------------
// Ethernet driver — W5500 via SPI2.
//
// eth_driver_init() is non-blocking: it starts the SPI/W5500 driver and
// DHCP negotiation asynchronously.  When an IP address is obtained, the bit
// ETH_IP_READY_BIT is set in the returned event group.  mDNS is also started
// at that point (hostname "esp32-zigbee").
// ---------------------------------------------------------------------------

#define ETH_IP_READY_BIT  BIT0

typedef struct {
    bool started;
    bool link_up;
    bool has_ip;
    char mac[18];
    char ip[16];
    char netmask[16];
    char gateway[16];
} eth_driver_status_t;

/**
 * Initialise the default event loop, esp_netif, SPI2 bus, W5500 MAC/PHY
 * and register IP event handlers.
 *
 * Returns the event group; ETH_IP_READY_BIT is set when DHCP completes.
 */
EventGroupHandle_t eth_driver_init(void);

/** Re-apply the current mDNS hostname and instance from app_config. */
void eth_driver_apply_mdns_config(void);

/** Return the last known Ethernet status snapshot. */
void eth_driver_get_status(eth_driver_status_t *out);
