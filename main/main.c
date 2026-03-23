#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "led_status.h"
#include "serial_cmd.h"
#include "timebase.h"
#include "zb_coordinator.h"
#include "zb_persistence.h"

static const char *TAG = "app_main";
static const gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_28;

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
    ESP_LOGI(TAG, "[T+%07.3f] EVT %s", timebase_now_s(), event_name);
}

void app_main(void)
{
    ESP_ERROR_CHECK(zb_persistence_init());
    timebase_init();
    device_table_init();
    led_status_init();
    led_status_set_base(LED_BASE_BOOT);

    zb_persist_state_t saved = {0};
    saved.version = ZB_PERSIST_VERSION;
    ESP_ERROR_CHECK(zb_persistence_load(&saved));

    ESP_LOGI(TAG, "[T+%07.3f] Boot coordinador Zigbee base", timebase_now_s());
    ESP_ERROR_CHECK(zb_coordinator_init(&saved, on_zb_event));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    serial_cmd_start();

    zb_persist_state_t runtime = {0};
    double last_persist_s = 0.0;
    bool permit_join_open = false;
    TickType_t permit_join_deadline = 0;
    bool button_prev_pressed = false;
    TickType_t last_button_tick = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(120);
    while (true) {
        ESP_ERROR_CHECK(zb_coordinator_poll());

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

        if (permit_join_open && permit_join_deadline != 0 && (int32_t)(now_tick - permit_join_deadline) >= 0) {
            permit_join_open = false;
            permit_join_deadline = 0;
            (void)zb_coordinator_set_permit_join(false);
            led_status_set_base(LED_BASE_READY_CLOSED);
            ESP_LOGI(TAG, "[T+%07.3f] Permit join expirado (180s) -> CERRADO", timebase_now_s());
        }

        const double now_s = timebase_now_s();
        if ((now_s - last_persist_s) >= 5.0) {
            if (zb_coordinator_get_runtime_state(&runtime) == ESP_OK) {
                (void)zb_persistence_save(&runtime);
            }
            last_persist_s = now_s;
        }

        led_status_poll();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
