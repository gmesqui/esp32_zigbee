#include "time_sync.h"
#include "eth_driver.h"
#include "utils.h"

#include <time.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "freertos/task.h"

#define TIME_SYNC_TASK_STACK      4096
#define TIME_SYNC_TASK_PRIORITY   4
#define TIME_SYNC_WAIT_MS         15000

static EventGroupHandle_t s_eth_eg;
static bool s_sntp_started = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ZB_LOG("TIME: SNTP synchronization completed");
}

static void time_sync_task(void *arg)
{
    (void)arg;

    ZB_LOG("TIME: waiting for Ethernet IP before SNTP sync");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
    config.sync_cb = time_sync_notification_cb;
    config.renew_servers_after_new_IP = true;
    config.ip_event_to_renew = IP_EVENT_ETH_GOT_IP;

    if (!s_sntp_started) {
        ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
        s_sntp_started = true;
        ZB_LOG("TIME: SNTP started with server time.google.com");
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_WAIT_MS));
    if (err == ESP_OK) {
        ZB_LOG("TIME: system clock is valid");
    } else if (err == ESP_ERR_NOT_FINISHED) {
        ZB_LOG("TIME: synchronization in progress, using interim system clock");
    } else {
        ZB_LOG("TIME: initial sync timed out, logs keep uptime until clock is valid");
    }

    vTaskDelete(NULL);
}

void time_sync_init(EventGroupHandle_t eth_ready_eg)
{
    s_eth_eg = eth_ready_eg;
    configASSERT(s_eth_eg);

    BaseType_t ok = xTaskCreate(time_sync_task, "time_sync_task",
                                TIME_SYNC_TASK_STACK, NULL,
                                TIME_SYNC_TASK_PRIORITY, NULL);
    configASSERT(ok == pdPASS);
}
