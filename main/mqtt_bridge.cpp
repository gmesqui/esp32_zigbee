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
 * Notas:
 * - Este firmware usa solo Ethernet por SPI; WiFi queda desactivado en sdkconfig.
 * - El RESET del modulo se conmuta manualmente desde GPIO25 antes de instalar esp_eth.
 * - ENC28J60 no trae MAC valida de fabrica; se deriva una MAC local desde la eFuse del ESP32-C5.
 * - Se usa un reloj SPI conservador (10 MHz) y se ajusta el CS hold time segun el driver oficial.
 * - Al arrancar mDNS pueden verse logs de "add mac filter not supported" desde esp_eth/esp_netif.
 *   En este driver ENC28J60 el filtro multicast fino no se implementa, pero el chip ya se configura
 *   para aceptar trafico multicast, asi que mDNS puede seguir funcionando igualmente.
 */

#include "mqtt_bridge.h"

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
#include "mdns.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "timebase.h"

namespace mqtt_bridge {

static const char *TAG = "mqtt_bridge";

static constexpr spi_host_device_t kSpiHost = SPI2_HOST;
static constexpr int kSpiClockMHz = 10;
static constexpr gpio_num_t kSpiMosiGpio = GPIO_NUM_4;
static constexpr gpio_num_t kSpiMisoGpio = GPIO_NUM_5;
static constexpr gpio_num_t kSpiSclkGpio = GPIO_NUM_6;
static constexpr gpio_num_t kSpiCsGpio = GPIO_NUM_23;
static constexpr gpio_num_t kIntGpio = GPIO_NUM_24;
static constexpr gpio_num_t kResetGpio = GPIO_NUM_25;

class Bridge {
public:
    esp_err_t init();

private:
    esp_err_t ensure_network_stack();
    esp_err_t register_event_handlers();
    esp_err_t hardware_reset();
    esp_err_t init_spi_bus();
    esp_err_t init_ethernet();
    esp_err_t attach_netif();
    esp_err_t start_mdns();
    esp_err_t start_mqtt();

    static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void on_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    bool initialized_ = false;
    bool handlers_registered_ = false;
    bool spi_bus_ready_ = false;
    bool mdns_started_ = false;
    bool mqtt_started_ = false;
    esp_eth_handle_t eth_handle_ = nullptr;
    esp_eth_mac_t *mac_ = nullptr;
    esp_eth_phy_t *phy_ = nullptr;
    esp_netif_t *netif_ = nullptr;
    esp_eth_netif_glue_handle_t eth_glue_ = nullptr;
    esp_mqtt_client_handle_t mqtt_client_ = nullptr;
    esp_event_handler_instance_t eth_event_instance_ = nullptr;
    esp_event_handler_instance_t got_ip_instance_ = nullptr;
    esp_event_handler_instance_t lost_ip_instance_ = nullptr;
};

static Bridge s_bridge;

static const char *enc28j60_rev_to_str(eth_enc28j60_rev_t rev)
{
    switch (rev) {
    case ENC28J60_REV_B1:
        return "B1";
    case ENC28J60_REV_B4:
        return "B4";
    case ENC28J60_REV_B5:
        return "B5";
    case ENC28J60_REV_B7:
        return "B7";
    default:
        return "unknown";
    }
}

esp_err_t Bridge::ensure_network_stack()
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

esp_err_t Bridge::register_event_handlers()
{
    if (handlers_registered_) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &Bridge::on_eth_event, this, &eth_event_instance_),
        TAG, "No se pudo registrar handler de ETH_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &Bridge::on_ip_event, this, &got_ip_instance_),
        TAG, "No se pudo registrar handler de IP_EVENT_ETH_GOT_IP");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &Bridge::on_ip_event, this, &lost_ip_instance_),
        TAG, "No se pudo registrar handler de IP_EVENT_ETH_LOST_IP");

    handlers_registered_ = true;
    return ESP_OK;
}

esp_err_t Bridge::hardware_reset()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << kResetGpio);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "No se pudo configurar GPIO de RESET del ENC28J60");

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(kResetGpio, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(kResetGpio, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t Bridge::init_spi_bus()
{
    if (spi_bus_ready_) {
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

    spi_bus_ready_ = true;
    return ESP_OK;
}

esp_err_t Bridge::init_ethernet()
{
    if (eth_handle_ != nullptr) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "Reset del ENC28J60 fallido");
    ESP_RETURN_ON_ERROR(init_spi_bus(), TAG, "No se pudo inicializar el bus SPI del ENC28J60");

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

    mac_ = esp_eth_mac_new_enc28j60(&enc_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac_ != nullptr, ESP_FAIL, TAG, "No se pudo crear MAC ENC28J60");
    phy_ = esp_eth_phy_new_enc28j60(&phy_config);
    ESP_RETURN_ON_FALSE(phy_ != nullptr, ESP_FAIL, TAG, "No se pudo crear PHY ENC28J60");

    ESP_LOGI(TAG, "[T+%07.3f] ENC28J60 detectado, revision %s", timebase_now_s(),
             enc28j60_rev_to_str(emac_enc28j60_get_chip_info(mac_)));

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac_, phy_);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle_), TAG, "Fallo instalando esp_eth");

    uint8_t base_mac[ETH_ADDR_LEN] = {};
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(base_mac), TAG, "No se pudo leer la MAC base de eFuse");
    uint8_t local_mac[ETH_ADDR_LEN] = {};
    esp_derive_local_mac(local_mac, base_mac);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle_, ETH_CMD_S_MAC_ADDR, local_mac), TAG,
                        "No se pudo asignar MAC local al ENC28J60");

    return ESP_OK;
}

esp_err_t Bridge::attach_netif()
{
    if (netif_ != nullptr) {
        return ESP_OK;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_ = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(netif_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear esp_netif Ethernet");

    eth_glue_ = esp_eth_new_netif_glue(eth_handle_);
    ESP_RETURN_ON_FALSE(eth_glue_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear esp_eth netif glue");

    ESP_RETURN_ON_ERROR(esp_netif_attach(netif_, eth_glue_), TAG, "No se pudo enlazar esp_eth con esp_netif");
    return ESP_OK;
}

esp_err_t Bridge::start_mdns()
{
    if (mdns_started_) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "No se pudo iniciar mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_MQTT_BRIDGE_MDNS_HOSTNAME), TAG,
                        "No se pudo configurar el hostname mDNS");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(CONFIG_MQTT_BRIDGE_MDNS_INSTANCE), TAG,
                        "No se pudo configurar la instancia mDNS");

    mdns_started_ = true;
    ESP_LOGI(TAG, "[T+%07.3f] mDNS activo como %s.local (%s)", timebase_now_s(),
             CONFIG_MQTT_BRIDGE_MDNS_HOSTNAME, CONFIG_MQTT_BRIDGE_MDNS_INSTANCE);
    return ESP_OK;
}

esp_err_t Bridge::start_mqtt()
{
    if (mqtt_started_) {
        return ESP_OK;
    }

    if (strlen(CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI) == 0) {
        ESP_LOGW(TAG, "[T+%07.3f] MQTT deshabilitado: CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI vacio", timebase_now_s());
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI;
    mqtt_cfg.credentials.client_id = CONFIG_MQTT_BRIDGE_MQTT_CLIENT_ID;

    if (mqtt_client_ == nullptr) {
        mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
        ESP_RETURN_ON_FALSE(mqtt_client_ != nullptr, ESP_FAIL, TAG, "No se pudo crear el cliente MQTT");
        ESP_RETURN_ON_ERROR(
            esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY, &Bridge::on_mqtt_event, this),
            TAG, "No se pudo registrar el handler del cliente MQTT");
    }

    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(mqtt_client_), TAG, "No se pudo arrancar el cliente MQTT");
    mqtt_started_ = true;
    ESP_LOGI(TAG, "[T+%07.3f] MQTT arrancado contra %s", timebase_now_s(), CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI);
    return ESP_OK;
}

esp_err_t Bridge::init()
{
    if (initialized_) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_network_stack(), TAG, "No se pudo preparar esp_netif/esp_event");
    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "No se pudieron registrar los eventos de red");
    ESP_RETURN_ON_ERROR(init_ethernet(), TAG, "No se pudo inicializar ENC28J60");
    ESP_RETURN_ON_ERROR(attach_netif(), TAG, "No se pudo adjuntar el netif Ethernet");
    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle_), TAG, "No se pudo arrancar esp_eth");

    initialized_ = true;
    ESP_LOGI(TAG, "[T+%07.3f] Bridge listo: Ethernet SPI ENC28J60 arrancado, esperando DHCP", timebase_now_s());
    return ESP_OK;
}

void Bridge::on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    Bridge *self = static_cast<Bridge *>(arg);
    if (self == nullptr || event_data == nullptr) {
        return;
    }

    uint8_t mac_addr[ETH_ADDR_LEN] = {};
    esp_eth_handle_t eth_handle = *static_cast<esp_eth_handle_t *>(event_data);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
            ESP_LOGI(TAG,
                     "[T+%07.3f] Ethernet link UP, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     timebase_now_s(),
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        } else {
            ESP_LOGI(TAG, "[T+%07.3f] Ethernet link UP", timebase_now_s());
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

void Bridge::on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    Bridge *self = static_cast<Bridge *>(arg);
    if (self == nullptr || event_data == nullptr) {
        return;
    }

    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        if (event->esp_netif != self->netif_) {
            return;
        }

        const esp_netif_ip_info_t *ip_info = &event->ip_info;
        ESP_LOGI(TAG, "[T+%07.3f] DHCP OK - IP " IPSTR " MASK " IPSTR " GW " IPSTR,
                 timebase_now_s(),
                 IP2STR(&ip_info->ip), IP2STR(&ip_info->netmask), IP2STR(&ip_info->gw));

        esp_err_t mdns_err = self->start_mdns();
        if (mdns_err != ESP_OK) {
            ESP_LOGE(TAG, "[T+%07.3f] Error arrancando mDNS: %s", timebase_now_s(), esp_err_to_name(mdns_err));
            return;
        }

        esp_err_t mqtt_err = self->start_mqtt();
        if (mqtt_err != ESP_OK) {
            ESP_LOGE(TAG, "[T+%07.3f] Error arrancando MQTT: %s", timebase_now_s(), esp_err_to_name(mqtt_err));
        }
        return;
    }

    if (event_id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet ha perdido la IP DHCP", timebase_now_s());
    }
}

void Bridge::on_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (event == nullptr) {
        return;
    }

    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[T+%07.3f] MQTT conectado", timebase_now_s());
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[T+%07.3f] MQTT desconectado", timebase_now_s());
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle != nullptr &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "[T+%07.3f] MQTT error TCP/TLS esp_err=0x%x tls=0x%x errno=%d",
                     timebase_now_s(),
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "[T+%07.3f] MQTT error genérico", timebase_now_s());
        }
        break;
    default:
        break;
    }
}

} // namespace mqtt_bridge

extern "C" esp_err_t mqtt_bridge_init(void)
{
    return mqtt_bridge::s_bridge.init();
}
