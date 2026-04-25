#include <stdio.h>
#include "board_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "utils.h"
#include "app_config.h"
#include "device_manager.h"
#include "nvs_cache.h"
#include "zcl_handler.h"
#include "device_interview.h"
#include "led_driver.h"
#include "button_handler.h"
#include "serial_cmd.h"
#include "zigbee_core.h"
#include "zb_events.h"
#include "eth_driver.h"
#include "time_sync.h"
#include "tcp_console.h"
#include "client_events.h"
#include "ws_transport.h"

// ---------------------------------------------------------------------------
// app_main — called by ESP-IDF after system init
// ---------------------------------------------------------------------------

void app_main(void)
{
    // 1. Initialise NVS flash (required by Zigbee stack and our own cache)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or has incompatible schema — erase and retry
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ZB_LOG("=== %s Zigbee Coordinator booting (%s) ===", BOARD_SOC_NAME, BOARD_NAME);
    ZB_LOG("Build: " __DATE__ " " __TIME__);

    // 2. Application configuration stored in NVS
    app_config_init();

    // 3. Zigbee event bus (neutral layer; must be before any emitters or consumers)
    zb_events_init();

    // 3. Ethernet driver (non-blocking; starts DHCP asynchronously)
    EventGroupHandle_t eth_eg = eth_driver_init();

    // 4. Time sync (starts SNTP after ETH_IP_READY_BIT)
    time_sync_init(eth_eg);

    // 5. Device manager (must be before NVS load and Zigbee init)
    dm_init();

    // 6. Load persisted device table from NVS
    //    Devices are restored with state=INTERVIEWED, online=false.
    //    They become online again when we receive traffic from them.
    if (nvs_cache_load()) {
        ZB_LOG("Device cache loaded: %u device(s)", dm_count());
    } else {
        ZB_LOG("No device cache — starting fresh");
    }

    // 7. ZCL handler (attribute cache, pending buffer)
    zcl_handler_init();

    // 8. WebSocket transport and neutral client event bridge
    tcp_console_init(eth_eg);
    ws_transport_init(eth_eg);
    client_events_init();

    // 9. Interview subsystem
    di_init();

    // 10. LED driver — starts the led_task
    led_driver_init();

    // 11. Button handler — starts the btn_task
    button_handler_init();

    // 12. Serial command handler — starts the serial_cmd_task
    serial_cmd_init();

    // 13. Zigbee core — starts the zigbee_main task
    //    The task initialises the stack and enters the main loop.
    //    All further operation is event-driven from there.
    zigbee_core_init();

    ZB_LOG("app_main: all subsystems started, handing off to event loop");

    // app_main returns here; all work is done in FreeRTOS tasks.
    // ESP-IDF will delete the main task automatically.
}
