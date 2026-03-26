#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "bridge_core.h"
#include "ethernet_link.h"
#include "led_status.h"
#include "matter_bridge.h"
#include "serial_cmd.h"
#include "timebase.h"
#include "zb_coordinator.h"

static const char *TAG = "app_main";
static const gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_28;
static const TickType_t DEVICE_TABLE_PERSIST_PERIOD_TICKS = pdMS_TO_TICKS(250);
static const uint32_t DEVICE_TABLE_PERSIST_TASK_STACK = 4096;
static const UBaseType_t DEVICE_TABLE_PERSIST_TASK_PRIO = 4;
static const TickType_t MATTER_INIT_DELAY_TICKS = pdMS_TO_TICKS(2000);

static void device_table_persist_task(void *arg)
{
    (void)arg;
    while (true) {
        device_table_persist_cache();
        vTaskDelay(DEVICE_TABLE_PERSIST_PERIOD_TICKS);
    }
}

static void disable_wifi_radio(void)
{
#if CONFIG_ESP_WIFI_ENABLED
    const esp_err_t stop_err = esp_wifi_stop();
    if (stop_err == ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] WiFi stop forzado al arrancar", timebase_now_s());
    } else if (stop_err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "[T+%07.3f] esp_wifi_stop devolvio %s", timebase_now_s(), esp_err_to_name(stop_err));
    }

    const esp_err_t deinit_err = esp_wifi_deinit();
    if (deinit_err == ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] WiFi deinit forzado al arrancar", timebase_now_s());
    } else if (deinit_err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGI(TAG, "[T+%07.3f] WiFi ya estaba desactivado (driver no inicializado)", timebase_now_s());
    } else {
        ESP_LOGW(TAG, "[T+%07.3f] esp_wifi_deinit devolvio %s", timebase_now_s(), esp_err_to_name(deinit_err));
    }
#else
    ESP_LOGI(TAG, "[T+%07.3f] WiFi compilado como desactivado", timebase_now_s());
#endif
}

static void log_memory_snapshot(const char *stage)
{
    ESP_LOGI(TAG, "[T+%07.3f] Mem %s: heap=%u internal=%u",
             timebase_now_s(), stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void on_zb_event(const char *event_name)
{
    if (event_name != NULL && strstr(event_name, "FROM_RX_MSG_") == event_name) {
        return;
    }
    if (event_name != NULL) {
        if (strstr(event_name, "INTERVIEW") != NULL || strstr(event_name, "CONFIG") != NULL) {
            led_status_pulse(LED_PULSE_INTERVIEW);
        } else if (strstr(event_name, "DEVICE_ANNOUNCE") != NULL || strstr(event_name, "REPORT") != NULL ||
                   strstr(event_name, "DEVICE_UPDATE") != NULL) {
            led_status_pulse(LED_PULSE_EVENT);
        } else {
            led_status_pulse(LED_PULSE_NET);
        }

        if (strcmp(event_name, "STACK_STARTED") == 0 || strcmp(event_name, "BDB_FORMATION_START") == 0 ||
            strcmp(event_name, "FACTORY_NEW_FORMATION") == 0) {
            led_status_set_base(LED_BASE_FORMING);
        } else if (strcmp(event_name, "NETWORK_FORMED") == 0 || strcmp(event_name, "NETWORK_RESTORED") == 0 ||
                   strcmp(event_name, "PERMIT_JOIN_CLOSED") == 0) {
            led_status_set_base(LED_BASE_READY_CLOSED);
        } else if (strcmp(event_name, "PERMIT_JOIN_OPEN") == 0) {
            led_status_set_base(LED_BASE_READY_OPEN);
        }
    }
    bridge_core_request_sync();
    ESP_LOGI(TAG, "[T+%07.3f] EVT %s", timebase_now_s(), event_name);
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    timebase_init();
    device_table_init();
    bridge_core_init();
    led_status_init();
    led_status_set_base(LED_BASE_BOOT);
    disable_wifi_radio();

    ESP_LOGI(TAG, "[T+%07.3f] Boot coordinador Zigbee base", timebase_now_s());
    log_memory_snapshot("boot");
    const esp_err_t ethernet_err = ethernet_link_init();
    if (ethernet_err != ESP_OK) {
        ESP_LOGE(TAG, "[T+%07.3f] ethernet_link_init fallo: %s", timebase_now_s(), esp_err_to_name(ethernet_err));
    }
    log_memory_snapshot("post-ethernet");
    ESP_ERROR_CHECK(zb_coordinator_init(on_zb_event));
    log_memory_snapshot("post-zigbee-init");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    serial_cmd_start();
    xTaskCreate(device_table_persist_task, "device_table_persist", DEVICE_TABLE_PERSIST_TASK_STACK, NULL, DEVICE_TABLE_PERSIST_TASK_PRIO, NULL);

    bool permit_join_open = false;
    TickType_t permit_join_deadline = 0;
    bool button_prev_pressed = false;
    TickType_t last_button_tick = 0;
    bool zigbee_poll_degraded = false;
    bool matter_init_attempted = false;
    const TickType_t matter_init_deadline = xTaskGetTickCount() + MATTER_INIT_DELAY_TICKS;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(120);
    while (true) {
        const esp_err_t poll_err = zb_coordinator_poll();
        if (poll_err == ESP_OK) {
            if (zigbee_poll_degraded) {
                ESP_LOGI(TAG, "[T+%07.3f] Poll Zigbee recuperado", timebase_now_s());
                zigbee_poll_degraded = false;
            }
        } else if (poll_err == ESP_ERR_TIMEOUT || poll_err == ESP_ERR_INVALID_STATE) {
            if (!zigbee_poll_degraded) {
                ESP_LOGW(TAG, "[T+%07.3f] Poll Zigbee temporalmente degradado: %s", timebase_now_s(), esp_err_to_name(poll_err));
                zigbee_poll_degraded = true;
            }
        } else {
            ESP_ERROR_CHECK(poll_err);
        }

        const bool button_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
        const TickType_t now_tick = xTaskGetTickCount();
        if (button_pressed && !button_prev_pressed && (now_tick - last_button_tick) > debounce_ticks) {
            permit_join_open = !permit_join_open;
            if (permit_join_open) {
                (void)zb_coordinator_set_permit_join(true);
                permit_join_deadline = now_tick + pdMS_TO_TICKS(180000);
                led_status_set_base(LED_BASE_READY_OPEN);
                ESP_LOGI(TAG, "[T+%07.3f] BOOT pulsado -> permit join ABIERTO (180s)", timebase_now_s());
            } else {
                (void)zb_coordinator_set_permit_join(false);
                permit_join_deadline = 0;
                led_status_set_base(LED_BASE_READY_CLOSED);
                ESP_LOGI(TAG, "[T+%07.3f] BOOT pulsado -> permit join CERRADO", timebase_now_s());
            }
            last_button_tick = now_tick;
        }
        button_prev_pressed = button_pressed;

        if (!matter_init_attempted && (int32_t)(now_tick - matter_init_deadline) >= 0) {
            matter_init_attempted = true;
            log_memory_snapshot("pre-matter-init");
            const esp_err_t matter_err = matter_bridge_init();
            if (matter_err != ESP_OK) {
                ESP_LOGE(TAG, "[T+%07.3f] matter_bridge_init fallo: %s", timebase_now_s(), esp_err_to_name(matter_err));
            }
            log_memory_snapshot("post-matter-init");
        }

        if (permit_join_open && permit_join_deadline != 0 && (int32_t)(now_tick - permit_join_deadline) >= 0) {
            permit_join_open = false;
            permit_join_deadline = 0;
            (void)zb_coordinator_set_permit_join(false);
            led_status_set_base(LED_BASE_READY_CLOSED);
            ESP_LOGI(TAG, "[T+%07.3f] Permit join expirado (180s) -> CERRADO", timebase_now_s());
        }
        bridge_core_poll();
        matter_bridge_poll();
        led_status_poll();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
