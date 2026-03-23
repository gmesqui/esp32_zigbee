#include "serial_cmd.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_table.h"
#include "timebase.h"

static const char *TAG = "serial_cmd";

static void serial_cmd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[T+%07.3f] Comandos serie activos. Tecla '1' => volcado JSON", timebase_now_s());
    while (true) {
        int ch = fgetc(stdin);
        if (ch == '1') {
            ESP_LOGI(TAG, "[T+%07.3f] Comando recibido: 1 (dump JSON)", timebase_now_s());
            device_table_dump_json();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void serial_cmd_start(void)
{
    xTaskCreate(serial_cmd_task, "serial_cmd_task", 4096, NULL, 5, NULL);
}
