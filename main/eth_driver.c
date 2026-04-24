#include "eth_driver.h"
#include "board_config.h"
#include "utils.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_eth_eg;

// ---------------------------------------------------------------------------
// mDNS — called once after IP is obtained
// ---------------------------------------------------------------------------
static void mdns_init_once(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ZB_LOG("mDNS: init failed (%s)", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("esp32-zigbee");
    mdns_instance_name_set("ESP32 Zigbee Coordinator");
    ZB_LOG("mDNS: hostname=esp32-zigbee.local");
}

// ---------------------------------------------------------------------------
// IP event handler
// ---------------------------------------------------------------------------
static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ZB_LOG("ETH: IP " IPSTR " / GW " IPSTR,
               IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        ZB_LOG("ETH: free internal heap=%u bytes", (unsigned)free_internal);
        mdns_init_once();
        xEventGroupSetBits(s_eth_eg, ETH_IP_READY_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        ZB_LOG("ETH: IP lost");
        xEventGroupClearBits(s_eth_eg, ETH_IP_READY_BIT);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            if (data) {
                uint8_t mac_addr[6] = {0};
                esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)data;
                if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
                    ZB_LOG("ETH: link up, MAC %02X:%02X:%02X:%02X:%02X:%02X",
                           mac_addr[0], mac_addr[1], mac_addr[2],
                           mac_addr[3], mac_addr[4], mac_addr[5]);
                    break;
                }
            }
            ZB_LOG("ETH: link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ZB_LOG("ETH: link down");
            break;
        case ETHERNET_EVENT_START:
            ZB_LOG("ETH: started");
            break;
        case ETHERNET_EVENT_STOP:
            ZB_LOG("ETH: stopped");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
EventGroupHandle_t eth_driver_init(void)
{
    s_eth_eg = xEventGroupCreate();
    configASSERT(s_eth_eg);

    // 1. Default event loop (required by esp_eth and esp_netif)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. esp_netif
    ESP_ERROR_CHECK(esp_netif_init());

    // 3. Create default Ethernet netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    // 4. SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num   = ETH_MOSI_GPIO,
        .miso_io_num   = ETH_MISO_GPIO,
        .sclk_io_num   = ETH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 5. SPI Ethernet MAC drivers use gpio_isr_handler_add() on their INT pin.
    // Install the shared GPIO ISR service up front so W5500 init can hook it.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // 6. W5500 SPI device config
    spi_device_interface_config_t spi_devcfg = {
        .command_bits     = 16,
        .address_bits     = 8,
        .dummy_bits       = 0,
        .mode             = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz   = ETH_SPI_CLOCK_HZ,
        .input_delay_ns   = 0,
        .spics_io_num     = ETH_CS_GPIO,
        .queue_size       = 20,
    };

    // 7. W5500 MAC config
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_cfg.int_gpio_num = ETH_INT_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = ETH_RST_GPIO;
    phy_cfg.autonego_timeout_ms = 3000;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    assert(mac);
    assert(phy);

    // 8. Install Ethernet driver
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    // W5500 has no factory-burned MAC, so assign one derived from the chip base MAC.
    uint8_t eth_mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    // 9. Attach to netif
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    // 10. Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT,  ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   ESP_EVENT_ANY_ID,
                                                &ip_event_handler, NULL));

    // 11. Start Ethernet
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ZB_LOG("ETH: W5500 driver started on %s (MOSI=%d MISO=%d SCLK=%d CS=%d INT=%d RST=%d, waiting for DHCP)",
           BOARD_NAME, ETH_MOSI_GPIO, ETH_MISO_GPIO, ETH_SCLK_GPIO,
           ETH_CS_GPIO, ETH_INT_GPIO, ETH_RST_GPIO);

    return s_eth_eg;
}
