#include "time_sync.h"
#include "app_config.h"
#include "eth_driver.h"
#include "utils.h"

#include <time.h>
#include <sys/time.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "freertos/task.h"

#define TIME_SYNC_TASK_STACK      4096
#define TIME_SYNC_TASK_PRIORITY   4
#define TIME_SYNC_WAIT_MS         15000

static EventGroupHandle_t s_eth_eg;
static bool s_sntp_started = false;
static time_sync_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static void time_sync_update_config_status(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    snprintf(s_status.server, sizeof(s_status.server), "%s", cfg->ntp_server);
    snprintf(s_status.timezone, sizeof(s_status.timezone), "%s", cfg->timezone);
    portEXIT_CRITICAL(&s_status_lock);
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    uint32_t sync_s = utils_uptime_ms() / 1000u;
    portENTER_CRITICAL(&s_status_lock);
    s_status.synced = true;
    s_status.last_sync_uptime_s = sync_s;
    portEXIT_CRITICAL(&s_status_lock);
    ZB_LOG("TIME: SNTP synchronization completed");
}

static void time_sync_task(void *arg)
{
    (void)arg;

    ZB_LOG("TIME: waiting for Ethernet IP before SNTP sync");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    app_config_t cfg;
    app_config_get(&cfg);
    time_sync_update_config_status(&cfg);

    setenv("TZ", cfg.timezone, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg.ntp_server);
    config.sync_cb = time_sync_notification_cb;
    config.renew_servers_after_new_IP = true;
    config.ip_event_to_renew = IP_EVENT_ETH_GOT_IP;

    if (!s_sntp_started) {
        ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
        s_sntp_started = true;
        portENTER_CRITICAL(&s_status_lock);
        s_status.started = true;
        portEXIT_CRITICAL(&s_status_lock);
        ZB_LOG("TIME: SNTP started with server %s timezone %s",
               cfg.ntp_server, cfg.timezone);
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

    app_config_t cfg;
    app_config_get(&cfg);
    time_sync_update_config_status(&cfg);

    BaseType_t ok = xTaskCreate(time_sync_task, "time_sync_task",
                                TIME_SYNC_TASK_STACK, NULL,
                                TIME_SYNC_TASK_PRIORITY, NULL);
    configASSERT(ok == pdPASS);
}

void time_sync_restart(void)
{
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.started = false;
    s_status.synced = false;
    s_status.last_sync_uptime_s = 0;
    portEXIT_CRITICAL(&s_status_lock);

    BaseType_t ok = xTaskCreate(time_sync_task, "time_sync_task",
                                TIME_SYNC_TASK_STACK, NULL,
                                TIME_SYNC_TASK_PRIORITY, NULL);
    configASSERT(ok == pdPASS);
}

void time_sync_get_status(time_sync_status_t *out)
{
    if (!out) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    out->synced = out->synced || utils_wall_time_valid();
}
