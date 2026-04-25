#include "serial_cmd.h"
#include "app_config.h"
#include "device_manager.h"
#include "device_interview.h"
#include "nvs_cache.h"
#include "button_handler.h"
#include "report_config.h"
#include "utils.h"
#include "tcp_console.h"
#include "ws_protocol_selftest.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_zigbee_core.h"

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
#include "driver/uart.h"
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#else
#error "Unsupported console channel for serial command handler"
#endif

#define SERIAL_ZB_LOCK_WAIT_MS 1000u

// ---------------------------------------------------------------------------
// Console channel abstraction
// ---------------------------------------------------------------------------

static const char *console_channel_name(void)
{
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    return "UART0 + TCP";
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    return "USB Serial/JTAG + TCP";
#else
    return "TCP console";
#endif
}

static esp_err_t console_channel_init(void)
{
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    return uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    return usb_serial_jtag_driver_install(&usb_cfg);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static int console_read_bytes(uint8_t *buf, TickType_t timeout_ticks)
{
    if (!buf) {
        return 0;
    }

    const TickType_t poll_ticks = pdMS_TO_TICKS(20);
    TickType_t start_tick = xTaskGetTickCount();

    for (;;) {
        int n = 0;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
        n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, 1, 0);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
        n = usb_serial_jtag_read_bytes(buf, 1, 0);
#endif
        if (n > 0) {
            return n;
        }

        n = tcp_console_read_bytes(buf, 1, 0);
        if (n > 0) {
            return n;
        }

        if (timeout_ticks == 0 ||
            (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return 0;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        TickType_t remaining = timeout_ticks - elapsed;
        vTaskDelay(remaining < poll_ticks ? remaining : poll_ticks);
    }
}

#define printf(...) utils_console_printf(__VA_ARGS__)
#define putchar(c) utils_console_putchar(c)

// ---------------------------------------------------------------------------
// Interactive input helpers
// ---------------------------------------------------------------------------

/** Read a line from the active console into buf (blocking, echoed, no timeout). */
static void read_line(char *buf, size_t len)
{
    size_t pos = 0;
    memset(buf, 0, len);
    while (pos < len - 1) {
        uint8_t c;
        if (console_read_bytes(&c, pdMS_TO_TICKS(10000)) <= 0) break;
        if (c == '\r' || c == '\n') { putchar('\n'); break; }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) { pos--; printf("\b \b"); }
            continue;
        }
        buf[pos++] = (char)c;
        putchar(c);
    }
    buf[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void cmd_device_list(void)
{
    dm_lock();

    printf("DEVICE LIST\n");
    printf("  ts_s: %.3f\n", utils_uptime_s());
    printf("  device_count: %u\n", dm_count());

    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = dm_get_by_index(i);
        if (!d) continue;

        char ieee_str[20];
        utils_ieee_to_str(d->ieee_addr, ieee_str, sizeof(ieee_str));
        float last_s = d->last_seen_ms ? (float)d->last_seen_ms / 1000.0f : 0.0f;

        printf("  - device[%d]\n", i);
        printf("      ieee: %s\n", ieee_str);
        printf("      friendly_name: %s\n",
               d->friendly_name[0] ? d->friendly_name : "-");
        printf("      short: 0x%04X\n", d->nwk_addr);
        printf("      online: %s\n", d->online ? "true" : "false");
        printf("      is_sleepy: %s\n", d->is_sleepy ? "true" : "false");
        printf("      manufacturer: %s\n", d->manufacturer[0] ? d->manufacturer : "-");
        printf("      model: %s\n", d->model[0] ? d->model : "-");
        printf("      power_source: %s\n", utils_power_source_name(d->power_source));
        printf("      state: %s\n", utils_device_state_name((int)d->state));
        printf("      last_seen_s: %.3f\n", last_s);
        printf("      lqi: %u\n", d->last_lqi);
        printf("      rssi: %d\n", d->last_rssi);
        printf("      reporting_configured: %s\n",
               d->reporting_configured ? "true" : "false");
        printf("      endpoints:\n");
        for (int e = 0; e < d->endpoint_count; e++) {
            endpoint_record_t *ep = &d->endpoints[e];
            printf("        - endpoint[%d]\n", e);
            printf("            id: %u\n", ep->endpoint_id);
            printf("            profile: 0x%04X\n", ep->profile_id);
            printf("            device_id: %s\n", utils_device_type_name(ep->device_id));
            printf("            in_clusters:");
            if (ep->in_cluster_count == 0) {
                printf(" -");
            } else {
                for (int c = 0; c < ep->in_cluster_count; c++) {
                    printf("%s0x%04X", c == 0 ? " " : ", ", ep->in_clusters[c]);
                }
            }
            printf("\n");
            printf("            out_clusters:");
            if (ep->out_cluster_count == 0) {
                printf(" -");
            } else {
                for (int c = 0; c < ep->out_cluster_count; c++) {
                    printf("%s0x%04X", c == 0 ? " " : ", ", ep->out_clusters[c]);
                }
            }
            printf("\n");
        }
        printf("      stats:\n");
        printf("        report_attr_ok: %lu\n", (unsigned long)d->report_attr_ok);
        printf("        report_attr_unchanged: %lu\n",
               (unsigned long)d->report_attr_unchanged);
        printf("        read_rsp_ok: %lu\n", (unsigned long)d->read_rsp_ok);
        printf("        read_rsp_fail: %lu\n", (unsigned long)d->read_rsp_fail);
        printf("        interview_attempts: %lu\n",
               (unsigned long)d->interview_attempts);
    }
    dm_unlock();
    printf("END DEVICE LIST\n");
}

static void cmd_network_stats(void)
{
    uint8_t channel = 0;
    uint16_t pan_id = 0;
    esp_zb_ieee_addr_t ext_pan;
    esp_zb_ieee_addr_t coord_ieee;

    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(SERIAL_ZB_LOCK_WAIT_MS))) {
        ZB_PRINT("NETWORK stats unavailable: Zigbee lock timeout\n");
        return;
    }

    channel = esp_zb_get_current_channel();
    pan_id = esp_zb_get_pan_id();
    esp_zb_get_extended_pan_id(ext_pan);
    esp_zb_get_long_address(coord_ieee);
    esp_zb_lock_release();

    uint8_t online = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = dm_get_by_index(i);
        if (d && d->online) online++;
    }

    ZB_PRINT("NETWORK channel=%u pan_id=0x%04X devices=%u/%u online=%u permit_join=%s\n",
             channel, pan_id, dm_count(), MAX_DEVICES, online,
             button_handler_permit_join_active() ? "open" : "closed");

    ZB_PRINT("COORD_IEEE=");
    for (int i = 7; i >= 0; i--) printf("%02X", coord_ieee[i]);
    printf("\n");
}

static void cmd_task_list(void)
{
#if configUSE_TRACE_FACILITY == 1 && configGENERATE_RUN_TIME_STATS == 1
    static char task_buf[1024];
    vTaskList(task_buf);
    ZB_PRINT("TASKS:\nName             State  Pri  Stack  Num\n%s\n", task_buf);
#else
    ZB_PRINT("TASKS (trace not enabled - showing known tasks):\n"
             "  zigbee_main  led_task  serial_cmd_task  btn_task\n");
#endif
}

static void cmd_heap_stats(void)
{
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free = esp_get_minimum_free_heap_size();
    ZB_PRINT("HEAP free=%u min_free=%u\n",
             (unsigned)free_heap, (unsigned)min_free);
}

static void cmd_set_friendly_name(void)
{
    char ieee_str[24] = {0};
    char name_str[FRIENDLY_NAME_LEN] = {0};

    printf("Enter IEEE (e.g. 0x00124B00AABBCCDD): ");
    fflush(stdout);
    read_line(ieee_str, sizeof(ieee_str));

    uint64_t ieee = 0;
    if (!utils_str_to_ieee(ieee_str, &ieee)) {
        printf("Invalid IEEE address.\n");
        return;
    }

    dm_lock();
    device_record_t *dev = dm_find_by_ieee(ieee);
    dm_unlock();

    if (!dev) {
        printf("Device not found.\n");
        return;
    }

    printf("Current name: %s\n", dm_display_name(dev));
    printf("Enter new friendly name (max 32 chars): ");
    fflush(stdout);
    read_line(name_str, sizeof(name_str));

    if (name_str[0] == '\0') {
        printf("Name unchanged.\n");
        return;
    }

    dm_lock();
    dm_set_friendly_name(dev, name_str);
    dm_unlock();

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dm_get_by_index(i) == dev) {
            nvs_cache_save_device((uint8_t)i);
            break;
        }
    }
    printf("Name set to \"%s\"\n", dev->friendly_name);
}

static void cmd_reinterview(void)
{
    char ieee_str[24] = {0};
    printf("Enter IEEE to re-interview: ");
    fflush(stdout);
    read_line(ieee_str, sizeof(ieee_str));

    uint64_t ieee = 0;
    if (!utils_str_to_ieee(ieee_str, &ieee)) {
        printf("Invalid IEEE.\n");
        return;
    }

    dm_lock();
    device_record_t *dev = dm_find_by_ieee(ieee);
    dm_unlock();

    if (!dev) {
        printf("Device not found.\n");
        return;
    }

    dev->state = DEV_STATE_NEW;
    if (!di_enqueue_async(dev)) {
        printf("Unable to queue interview right now.\n");
        return;
    }

    printf("Re-interview queued for %s\n", dm_display_name(dev));
}

static void cmd_read_reporting_config(void)
{
    char ieee_str[24] = {0};
    char ep_str[8] = {0};
    char cluster_str[16] = {0};
    char attr_str[16] = {0};

    printf("Enter IEEE: ");
    fflush(stdout);
    read_line(ieee_str, sizeof(ieee_str));

    uint64_t ieee = 0;
    if (!utils_str_to_ieee(ieee_str, &ieee)) {
        printf("Invalid IEEE.\n");
        return;
    }

    dm_lock();
    device_record_t *dev = dm_find_by_ieee(ieee);
    dm_unlock();
    if (!dev) {
        printf("Device not found.\n");
        return;
    }

    printf("Enter endpoint (decimal): ");
    fflush(stdout);
    read_line(ep_str, sizeof(ep_str));
    long endpoint = strtol(ep_str, NULL, 10);
    if (endpoint <= 0 || endpoint > 255) {
        printf("Invalid endpoint.\n");
        return;
    }

    printf("Enter cluster hex (e.g. 0006): ");
    fflush(stdout);
    read_line(cluster_str, sizeof(cluster_str));
    long cluster_id = strtol(cluster_str, NULL, 16);
    if (cluster_id < 0 || cluster_id > 0xFFFF) {
        printf("Invalid cluster.\n");
        return;
    }

    printf("Enter attribute hex (e.g. 0000): ");
    fflush(stdout);
    read_line(attr_str, sizeof(attr_str));
    long attr_id = strtol(attr_str, NULL, 16);
    if (attr_id < 0 || attr_id > 0xFFFF) {
        printf("Invalid attribute.\n");
        return;
    }

    if (!rc_read_reporting_config_async(dev, (uint8_t)endpoint,
                                        (uint16_t)cluster_id,
                                        (uint16_t)attr_id)) {
        printf("Unable to queue Read Reporting Configuration.\n");
        return;
    }

    printf("Read Reporting Configuration queued for %s ep=%u cluster=0x%04lX attr=0x%04lX\n",
           dm_display_name(dev), (unsigned)endpoint, cluster_id, attr_id);
}

static void cmd_erase_cache(void)
{
    char confirm[8] = {0};
    printf("This will erase ALL cached devices. Type YES to confirm: ");
    fflush(stdout);
    read_line(confirm, sizeof(confirm));
    if (strcmp(confirm, "YES") != 0) {
        printf("Cancelled.\n");
        return;
    }
    nvs_cache_erase();
    printf("NVS device cache erased. Reboot to take effect.\n");
}

static void cmd_help(void)
{
    char prefix[32];
    utils_format_log_prefix(prefix, sizeof(prefix));

    printf(
        "[%s] KEY MAP:\n"
        "  1 - Device list\n"
        "  2 - Network statistics\n"
        "  3 - FreeRTOS task list\n"
        "  4 - Heap statistics\n"
        "  5 - Interview queue status\n"
        "  g - Read reporting config (interactive)\n"
        "  w - WebSocket protocol self-test\n"
        "  n - Set friendly name\n"
        "  j - Toggle permit join\n"
        "  r - Re-interview device\n"
        "  e - Erase NVS device cache\n"
        "  ? - This help\n",
        prefix);
}

// ---------------------------------------------------------------------------
// Main task
// ---------------------------------------------------------------------------

static void serial_cmd_task(void *arg)
{
    (void)arg;
    uint8_t c;

    ZB_PRINT("Serial command task ready on %s. Press '?' for help.\n",
             console_channel_name());

    for (;;) {
        int n = console_read_bytes(&c, pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        switch ((char)c) {
            case '1': cmd_device_list(); break;
            case '2': cmd_network_stats(); break;
            case '3': cmd_task_list(); break;
            case '4': cmd_heap_stats(); break;
            case '5':
                ZB_PRINT("Interview: use logs to track state.\n");
                break;
            case 'g':
                cmd_read_reporting_config();
                break;
            case 'w':
                ws_protocol_selftest_run(true);
                break;
            case 'n':
                cmd_set_friendly_name();
                break;
            case 'j':
                if (!button_handler_permit_join_active()) {
                    app_config_t cfg;
                    app_config_get(&cfg);
                    button_handler_set_permit_join_duration((uint8_t)cfg.permit_join_duration_s);
                    ZB_LOG("PERMIT_JOIN OPEN via serial (%us)", cfg.permit_join_duration_s);
                } else {
                    button_handler_set_permit_join_duration(0);
                    ZB_LOG("PERMIT_JOIN CLOSED via serial");
                }
                break;
            case 'r':
                cmd_reinterview();
                break;
            case 'e':
                cmd_erase_cache();
                break;
            case '?':
                cmd_help();
                break;
            case '\r':
            case '\n':
                break;
            default:
                ZB_PRINT("Unknown key '\\x%02X'. Press '?' for help.\n", c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void serial_cmd_init(void)
{
    ESP_ERROR_CHECK(console_channel_init());

    xTaskCreate(serial_cmd_task, "serial_cmd_task", 4096, NULL, 3, NULL);
    ZB_LOG("Serial command handler init OK on %s", console_channel_name());
}
