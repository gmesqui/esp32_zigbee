#include "serial_cmd.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "bridge_core.h"
#include "matter_bridge.h"
#include "timebase.h"
#include "zb_coordinator.h"

static const char *TAG = "serial_cmd";

static void serial_cmd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "[T+%07.3f] Comandos serie activos. Teclas: '1'=dump JSON, '2'=borrar zb_cache + reboot, '3'=local reset Zigbee + borrar zb_cache, '4'=factory reset Zigbee + borrar zb_cache, '5'=reboot MCU, '6'=dump bridge registry, '7'=borrar bridge registry + reboot, '8'=factory reset Zigbee + borrar zb_cache + bridge registry, '9'=factory reset Matter + reboot",
             timebase_now_s());
    while (true) {
        int ch = fgetc(stdin);
        if (ch == '1') {
            ESP_LOGI(TAG, "[T+%07.3f] Comando recibido: 1 (dump JSON)", timebase_now_s());
            device_table_dump_json();
        } else if (ch == '2') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 2 (borrar zb_cache + reboot)", timebase_now_s());
            device_table_clear_cache_and_runtime();
            ESP_LOGW(TAG, "[T+%07.3f] Reinicio MCU tras borrar zb_cache", timebase_now_s());
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (ch == '3') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 3 (local reset Zigbee + borrar zb_cache)", timebase_now_s());
            device_table_clear_cache_and_runtime();
            const esp_err_t err = zb_coordinator_local_reset();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[T+%07.3f] Local reset no ejecutado: %s", timebase_now_s(), esp_err_to_name(err));
            }
        } else if (ch == '4') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 4 (factory reset Zigbee + borrar zb_cache)", timebase_now_s());
            device_table_clear_cache_and_runtime();
            zb_coordinator_factory_reset();
        } else if (ch == '5') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 5 (reboot MCU)", timebase_now_s());
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (ch == '6') {
            ESP_LOGI(TAG, "[T+%07.3f] Comando recibido: 6 (dump bridge registry)", timebase_now_s());
            bridge_core_dump_registry_json();
        } else if (ch == '7') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 7 (borrar bridge registry + reboot)", timebase_now_s());
            bridge_core_reset_registry();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (ch == '8') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 8 (factory reset Zigbee + borrar zb_cache + bridge registry)", timebase_now_s());
            bridge_core_reset_registry();
            device_table_clear_cache_and_runtime();
            zb_coordinator_factory_reset();
        } else if (ch == '9') {
            ESP_LOGW(TAG, "[T+%07.3f] Comando recibido: 9 (factory reset Matter + reboot)", timebase_now_s());
            matter_bridge_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void serial_cmd_start(void)
{
    /* JSON grande + printf de flotantes requiere mas stack que 4KB. */
    xTaskCreate(serial_cmd_task, "serial_cmd_task", 12288, NULL, 5, NULL);
}
