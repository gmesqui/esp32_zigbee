/*
 * ENC28J60 <-> ESP32-C5-DevKitC cableado para el coordinador:
 *
 *   SI (MOSI)  -> GPIO4
 *   SO (MISO)  -> GPIO5
 *   SCK        -> GPIO6
 *   CS         -> GPIO23
 *   INT        -> GPIO24
 *   RESET      -> GPIO25
 *   VCC        -> 3V3
 *   GND        -> GND
 *
 * WOL y CLOCKOUT: no conectar.
 *
 * Este modulo levanta solo la conectividad Ethernet necesaria para Matter.
 */

#include "ethernet_link.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_enc28j60.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "timebase.h"

static const char *TAG = "ethernet_link";
static constexpr spi_host_device_t kSpiHost = SPI2_HOST;
static constexpr int kSpiClockMHz = 10;
static constexpr gpio_num_t kSpiMosiGpio = GPIO_NUM_4;
static constexpr gpio_num_t kSpiMisoGpio = GPIO_NUM_5;
static constexpr gpio_num_t kSpiSclkGpio = GPIO_NUM_6;
static constexpr gpio_num_t kSpiCsGpio = GPIO_NUM_23;
static constexpr gpio_num_t kIntGpio = GPIO_NUM_24;
static constexpr gpio_num_t kResetGpio = GPIO_NUM_25;

static bool s_handlers_registered = false;
static bool s_spi_bus_ready = false;
static esp_eth_handle_t s_eth_handle = nullptr;
static esp_eth_mac_t *s_mac = nullptr;
static esp_eth_phy_t *s_phy = nullptr;
static esp_netif_t *s_netif = nullptr;
static esp_eth_netif_glue_handle_t s_eth_glue = nullptr;
static esp_event_handler_instance_t s_eth_event_instance = nullptr;
static esp_event_handler_instance_t s_got_ip_instance = nullptr;
static esp_event_handler_instance_t s_lost_ip_instance = nullptr;

static const char *enc28j60_rev_to_str(eth_enc28j60_rev_t rev)
{
    switch (rev) {
    case ENC28J60_REV_B1: return "B1";
    case ENC28J60_REV_B4: return "B4";
    case ENC28J60_REV_B5: return "B5";
    case ENC28J60_REV_B7: return "B7";
    default: return "unknown";
    }
}

static esp_err_t ensure_network_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_data == nullptr) {
        return;
    }

    uint8_t mac_addr[ETH_ADDR_LEN] = {};
    esp_eth_handle_t eth_handle = *static_cast<esp_eth_handle_t *>(event_data);
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
            ESP_LOGI(TAG, "[T+%07.3f] Ethernet link UP, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     timebase_now_s(), mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet link DOWN", timebase_now_s());
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[T+%07.3f] Ethernet START", timebase_now_s());
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet STOP", timebase_now_s());
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_data == nullptr) {
        return;
    }

    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        if (event->esp_netif != s_netif) {
            return;
        }
        ESP_LOGI(TAG, "[T+%07.3f] DHCP OK - IP " IPSTR " MASK " IPSTR " GW " IPSTR,
                 timebase_now_s(), IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet ha perdido la IP DHCP", timebase_now_s());
    }
}

static esp_err_t register_event_handlers(void)
{
    if (s_handlers_registered) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, nullptr, &s_eth_event_instance),
        TAG, "No se pudo registrar ETH_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ip_event, nullptr, &s_got_ip_instance),
        TAG, "No se pudo registrar ETH_GOT_IP");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &on_ip_event, nullptr, &s_lost_ip_instance),
        TAG, "No se pudo registrar ETH_LOST_IP");
    s_handlers_registered = true;
    return ESP_OK;
}

static esp_err_t hardware_reset(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << kResetGpio);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "No se pudo configurar RESET ENC28J60");
    gpio_set_level(kResetGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kResetGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t init_spi_bus(void)
{
    if (s_spi_bus_ready) {
        return ESP_OK;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = kSpiMosiGpio;
    buscfg.miso_io_num = kSpiMisoGpio;
    buscfg.sclk_io_num = kSpiSclkGpio;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    err = spi_bus_initialize(kSpiHost, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_spi_bus_ready = true;
    return ESP_OK;
}

static esp_err_t init_ethernet_driver(void)
{
    if (s_eth_handle != nullptr) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "Reset ENC28J60 fallido");
    ESP_RETURN_ON_ERROR(init_spi_bus(), TAG, "No se pudo inicializar SPI ENC28J60");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = -1;
    phy_config.autonego_timeout_ms = 0;

    spi_device_interface_config_t spi_devcfg = {};
    spi_devcfg.mode = 0;
    spi_devcfg.clock_speed_hz = kSpiClockMHz * 1000 * 1000;
    spi_devcfg.spics_io_num = kSpiCsGpio;
    spi_devcfg.queue_size = 20;
    spi_devcfg.cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(kSpiClockMHz);

    eth_enc28j60_config_t enc_config = ETH_ENC28J60_DEFAULT_CONFIG(kSpiHost, &spi_devcfg);
    enc_config.int_gpio_num = kIntGpio;

    s_mac = esp_eth_mac_new_enc28j60(&enc_config, &mac_config);
    ESP_RETURN_ON_FALSE(s_mac != nullptr, ESP_FAIL, TAG, "No se pudo crear MAC ENC28J60");
    s_phy = esp_eth_phy_new_enc28j60(&phy_config);
    ESP_RETURN_ON_FALSE(s_phy != nullptr, ESP_FAIL, TAG, "No se pudo crear PHY ENC28J60");
    ESP_LOGI(TAG, "[T+%07.3f] ENC28J60 detectado, revision %s", timebase_now_s(),
             enc28j60_rev_to_str(emac_enc28j60_get_chip_info(s_mac)));

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_mac, s_phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "Fallo instalando esp_eth");

    uint8_t base_mac[ETH_ADDR_LEN] = {};
    uint8_t local_mac[ETH_ADDR_LEN] = {};
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(base_mac), TAG, "No se pudo leer MAC base");
    esp_derive_local_mac(local_mac, base_mac);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, local_mac), TAG, "No se pudo fijar MAC");
    return ESP_OK;
}

static esp_err_t attach_netif(void)
{
    if (s_netif != nullptr) {
        return ESP_OK;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear esp_netif");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, s_eth_glue), TAG, "No se pudo adjuntar netif");
    return ESP_OK;
}

extern "C" esp_err_t ethernet_link_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_network_stack(), TAG, "No se pudo preparar esp_netif/esp_event");
    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "No se pudieron registrar eventos");
    ESP_RETURN_ON_ERROR(init_ethernet_driver(), TAG, "No se pudo inicializar ENC28J60");
    ESP_RETURN_ON_ERROR(attach_netif(), TAG, "No se pudo adjuntar netif Ethernet");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "No se pudo arrancar esp_eth");
    ESP_LOGI(TAG, "[T+%07.3f] Ethernet listo: ENC28J60 arrancado, esperando DHCP", timebase_now_s());
    return ESP_OK;
}
