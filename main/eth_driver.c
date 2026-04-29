#include "eth_driver.h"
#include "app_config.h"
#include "board_config.h"
#include "utils.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_spec.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W5500_SPI_LOCK_TIMEOUT_MS 50
#define W5500_BSB_OFFSET          3
#define W5500_BSB_SOCK_TX_BUF_0   2
#define W5500_SPI_DMA_ALIGN       4
#define W5500_TX_DMA_BUF_SIZE     ((ETH_MAX_PACKET_SIZE + W5500_SPI_DMA_ALIGN - 1u) & ~(W5500_SPI_DMA_ALIGN - 1u))

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_eth_eg;
static bool s_mdns_ready;
static eth_driver_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    spi_host_device_t host;
    const spi_device_interface_config_t *devcfg;
} w5500_spi_dma_config_t;

typedef struct {
    spi_device_handle_t hdl;
    SemaphoreHandle_t lock;
    uint8_t *tx_dma_buf;
} w5500_spi_dma_ctx_t;

static bool w5500_is_tx_buffer_write(uint32_t addr)
{
    return ((addr >> W5500_BSB_OFFSET) & 0x1Fu) == W5500_BSB_SOCK_TX_BUF_0;
}

static void *w5500_spi_dma_init(const void *spi_config)
{
    const w5500_spi_dma_config_t *cfg = (const w5500_spi_dma_config_t *)spi_config;
    if (!cfg || !cfg->devcfg) {
        return NULL;
    }

    w5500_spi_dma_ctx_t *spi = calloc(1u, sizeof(*spi));
    if (!spi) {
        return NULL;
    }

    spi_device_interface_config_t devcfg = *cfg->devcfg;
    esp_err_t err = spi_bus_add_device(cfg->host, &devcfg, &spi->hdl);
    if (err != ESP_OK) {
        ZB_LOG("ETH: W5500 SPI add device failed (%s)", esp_err_to_name(err));
        free(spi);
        return NULL;
    }

    spi->lock = xSemaphoreCreateMutex();
    if (!spi->lock) {
        spi_bus_remove_device(spi->hdl);
        free(spi);
        return NULL;
    }

    spi->tx_dma_buf = heap_caps_aligned_alloc(W5500_SPI_DMA_ALIGN,
                                              W5500_TX_DMA_BUF_SIZE,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!spi->tx_dma_buf) {
        ZB_LOG("ETH: no DMA heap for W5500 TX staging buffer size=%u free_dma=%u largest_dma=%u",
               (unsigned)W5500_TX_DMA_BUF_SIZE,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        vSemaphoreDelete(spi->lock);
        spi_bus_remove_device(spi->hdl);
        free(spi);
        return NULL;
    }

    return spi;
}

static esp_err_t w5500_spi_dma_deinit(void *spi_ctx)
{
    w5500_spi_dma_ctx_t *spi = (w5500_spi_dma_ctx_t *)spi_ctx;
    if (!spi) {
        return ESP_OK;
    }

    if (spi->hdl) {
        spi_bus_remove_device(spi->hdl);
    }
    if (spi->lock) {
        vSemaphoreDelete(spi->lock);
    }
    free(spi->tx_dma_buf);
    free(spi);
    return ESP_OK;
}

static esp_err_t w5500_spi_dma_write(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                     const void *data, uint32_t data_len)
{
    w5500_spi_dma_ctx_t *spi = (w5500_spi_dma_ctx_t *)spi_ctx;
    if (!spi || !data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const void *tx_buffer = data;
    uint32_t tx_len = data_len;

    if (w5500_is_tx_buffer_write(addr)) {
        if (data_len > ETH_MAX_PACKET_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(spi->tx_dma_buf, data, data_len);
        tx_buffer = spi->tx_dma_buf;
        tx_len = (data_len + W5500_SPI_DMA_ALIGN - 1u) & ~(W5500_SPI_DMA_ALIGN - 1u);
        if (tx_len > data_len) {
            memset(spi->tx_dma_buf + data_len, 0, tx_len - data_len);
        }
    }

    spi_transaction_t trans = {
        .cmd = cmd,
        .addr = addr,
        .length = 8u * tx_len,
        .tx_buffer = tx_buffer,
    };

    if (xSemaphoreTake(spi->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = spi_device_polling_transmit(spi->hdl, &trans);
    xSemaphoreGive(spi->lock);
    if (err != ESP_OK) {
        ZB_LOG("ETH: W5500 SPI write failed len=%lu tx_len=%lu tx_dma=%u free_dma=%u largest_dma=%u err=%s",
               (unsigned long)data_len,
               (unsigned long)tx_len,
               (unsigned)esp_ptr_dma_capable(tx_buffer),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               esp_err_to_name(err));
    }
    return err;
}

static esp_err_t w5500_spi_dma_read(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                    void *data, uint32_t data_len)
{
    w5500_spi_dma_ctx_t *spi = (w5500_spi_dma_ctx_t *)spi_ctx;
    if (!spi || !data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t trans = {
        .flags = data_len <= 4u ? SPI_TRANS_USE_RXDATA : 0,
        .cmd = cmd,
        .addr = addr,
        .length = 8u * data_len,
        .rx_buffer = data,
    };

    if (xSemaphoreTake(spi->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = spi_device_polling_transmit(spi->hdl, &trans);
    xSemaphoreGive(spi->lock);
    if (err == ESP_OK && (trans.flags & SPI_TRANS_USE_RXDATA)) {
        memcpy(data, trans.rx_data, data_len);
    }
    return err;
}

static void status_set_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    snprintf(dst, dst_len, "%s", src);
}

static void format_mac(const uint8_t mac[6], char *buf, size_t len)
{
    if (!mac || !buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
// mDNS — called once after IP is obtained
// ---------------------------------------------------------------------------
void eth_driver_apply_mdns_config(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    if (!s_mdns_ready) {
        return;
    }

    mdns_hostname_set(cfg.mdns_hostname);
    mdns_instance_name_set(cfg.mdns_instance);
    ZB_LOG("mDNS: hostname=%s.local instance=\"%s\"",
           cfg.mdns_hostname, cfg.mdns_instance);
}

static void mdns_init_once(void)
{
    if (s_mdns_ready) {
        eth_driver_apply_mdns_config();
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ZB_LOG("mDNS: init failed (%s)", esp_err_to_name(err));
        return;
    }

    s_mdns_ready = true;
    eth_driver_apply_mdns_config();
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
        char ip[16];
        char gw[16];
        char netmask[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&e->ip_info.gw));
        snprintf(netmask, sizeof(netmask), IPSTR, IP2STR(&e->ip_info.netmask));
        portENTER_CRITICAL(&s_status_lock);
        s_status.has_ip = true;
        status_set_str(s_status.ip, sizeof(s_status.ip), ip);
        status_set_str(s_status.gateway, sizeof(s_status.gateway), gw);
        status_set_str(s_status.netmask, sizeof(s_status.netmask), netmask);
        portEXIT_CRITICAL(&s_status_lock);
        ZB_LOG("ETH: IP " IPSTR " / GW " IPSTR,
               IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        ZB_LOG("ETH: free internal heap=%u bytes", (unsigned)free_internal);
        mdns_init_once();
        xEventGroupSetBits(s_eth_eg, ETH_IP_READY_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        ZB_LOG("ETH: IP lost");
        portENTER_CRITICAL(&s_status_lock);
        s_status.has_ip = false;
        status_set_str(s_status.ip, sizeof(s_status.ip), "");
        status_set_str(s_status.gateway, sizeof(s_status.gateway), "");
        status_set_str(s_status.netmask, sizeof(s_status.netmask), "");
        portEXIT_CRITICAL(&s_status_lock);
        xEventGroupClearBits(s_eth_eg, ETH_IP_READY_BIT);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            portENTER_CRITICAL(&s_status_lock);
            s_status.link_up = true;
            portEXIT_CRITICAL(&s_status_lock);
            if (data) {
                uint8_t mac_addr[6] = {0};
                esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)data;
                if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
                    char mac_str[18];
                    format_mac(mac_addr, mac_str, sizeof(mac_str));
                    portENTER_CRITICAL(&s_status_lock);
                    status_set_str(s_status.mac, sizeof(s_status.mac), mac_str);
                    portEXIT_CRITICAL(&s_status_lock);
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
            portENTER_CRITICAL(&s_status_lock);
            s_status.link_up = false;
            s_status.has_ip = false;
            status_set_str(s_status.ip, sizeof(s_status.ip), "");
            status_set_str(s_status.gateway, sizeof(s_status.gateway), "");
            status_set_str(s_status.netmask, sizeof(s_status.netmask), "");
            portEXIT_CRITICAL(&s_status_lock);
            xEventGroupClearBits(s_eth_eg, ETH_IP_READY_BIT);
            break;
        case ETHERNET_EVENT_START:
            ZB_LOG("ETH: started");
            portENTER_CRITICAL(&s_status_lock);
            s_status.started = true;
            portEXIT_CRITICAL(&s_status_lock);
            break;
        case ETHERNET_EVENT_STOP:
            ZB_LOG("ETH: stopped");
            portENTER_CRITICAL(&s_status_lock);
            s_status.started = false;
            s_status.link_up = false;
            s_status.has_ip = false;
            portEXIT_CRITICAL(&s_status_lock);
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
        .queue_size       = 4,
    };

    w5500_spi_dma_config_t spi_dma_cfg = {
        .host = ETH_SPI_HOST,
        .devcfg = &spi_devcfg,
    };

    // 7. W5500 MAC config
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_cfg.int_gpio_num = ETH_INT_GPIO;
    w5500_cfg.custom_spi_driver.config = &spi_dma_cfg;
    w5500_cfg.custom_spi_driver.init = w5500_spi_dma_init;
    w5500_cfg.custom_spi_driver.deinit = w5500_spi_dma_deinit;
    w5500_cfg.custom_spi_driver.read = w5500_spi_dma_read;
    w5500_cfg.custom_spi_driver.write = w5500_spi_dma_write;

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
    char mac_str[18];
    format_mac(eth_mac, mac_str, sizeof(mac_str));
    portENTER_CRITICAL(&s_status_lock);
    status_set_str(s_status.mac, sizeof(s_status.mac), mac_str);
    portEXIT_CRITICAL(&s_status_lock);

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

void eth_driver_get_status(eth_driver_status_t *out)
{
    if (!out) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}
