#include "serial_cmd.h"
#include "device_manager.h"
#include "device_interview.h"
#include "nvs_cache.h"
#include "button_handler.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------------------
// Static output buffer — avoids large stack allocation
// ---------------------------------------------------------------------------
static char s_json_buf[6144];

// ---------------------------------------------------------------------------
// Interactive input helpers
// ---------------------------------------------------------------------------

/** Read a line from UART0 into buf (blocking, echoed, no timeout). */
static void read_line(char *buf, size_t len)
{
    size_t pos = 0;
    memset(buf, 0, len);
    while (pos < len - 1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(10000)) <= 0) break;
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

    char *p = s_json_buf;
    char *end = s_json_buf + sizeof(s_json_buf);

    p += snprintf(p, end - p,
        "{\"ts_s\":%.3f,\"device_count\":%u,\"devices\":[",
        utils_uptime_s(), dm_count());

    bool first = true;
    for (int i = 0; i < MAX_DEVICES && p < end - 256; i++) {
        device_record_t *d = dm_get_by_index(i);
        if (!d) continue;

        char ieee_str[20];
        utils_ieee_to_str(d->ieee_addr, ieee_str, sizeof(ieee_str));
        float last_s = d->last_seen_ms ? (float)d->last_seen_ms / 1000.0f : 0.0f;

        if (!first) p += snprintf(p, end - p, ",");
        first = false;

        p += snprintf(p, end - p,
            "{\"ieee\":\"%s\","
            "\"friendly_name\":\"%s\","
            "\"short\":\"0x%04X\","
            "\"online\":%s,"
            "\"is_sleepy\":%s,"
            "\"manufacturer\":\"%s\","
            "\"model\":\"%s\","
            "\"power_source\":\"%s\","
            "\"state\":\"%s\","
            "\"last_seen_s\":%.3f,"
            "\"lqi\":%u,"
            "\"rssi\":%d,"
            "\"reporting_configured\":%s,",
            ieee_str,
            d->friendly_name[0] ? d->friendly_name : "",
            d->nwk_addr,
            d->online ? "true" : "false",
            d->is_sleepy ? "true" : "false",
            d->manufacturer,
            d->model,
            utils_power_source_name(d->power_source),
            utils_device_state_name((int)d->state),
            last_s,
            d->last_lqi, d->last_rssi,
            d->reporting_configured ? "true" : "false");

        // Endpoints
        p += snprintf(p, end - p, "\"endpoints\":[");
        for (int e = 0; e < d->endpoint_count && p < end - 128; e++) {
            endpoint_record_t *ep = &d->endpoints[e];
            if (e) p += snprintf(p, end - p, ",");
            p += snprintf(p, end - p,
                "{\"id\":%u,\"profile\":\"0x%04X\","
                "\"device_id\":\"%s\","
                "\"in_clusters\":[",
                ep->endpoint_id, ep->profile_id,
                utils_device_type_name(ep->device_id));
            for (int c = 0; c < ep->in_cluster_count && p < end - 32; c++) {
                if (c) p += snprintf(p, end - p, ",");
                p += snprintf(p, end - p, "\"0x%04X\"", ep->in_clusters[c]);
            }
            p += snprintf(p, end - p, "],\"out_clusters\":[");
            for (int c = 0; c < ep->out_cluster_count && p < end - 32; c++) {
                if (c) p += snprintf(p, end - p, ",");
                p += snprintf(p, end - p, "\"0x%04X\"", ep->out_clusters[c]);
            }
            p += snprintf(p, end - p, "]}");
        }

        p += snprintf(p, end - p,
            "],\"stats\":{"
            "\"report_attr_ok\":%lu,"
            "\"report_attr_unchanged\":%lu,"
            "\"read_rsp_ok\":%lu,"
            "\"read_rsp_fail\":%lu,"
            "\"interview_attempts\":%lu}}",
            (unsigned long)d->report_attr_ok,
            (unsigned long)d->report_attr_unchanged,
            (unsigned long)d->read_rsp_ok,
            (unsigned long)d->read_rsp_fail,
            (unsigned long)d->interview_attempts);
    }

    p += snprintf(p, end - p, "]}");
    dm_unlock();

    printf("%s\n", s_json_buf);
}

static void cmd_network_stats(void)
{
    uint8_t channel = esp_zb_get_current_channel();
    uint16_t pan_id = esp_zb_get_pan_id();
    esp_zb_ieee_addr_t ext_pan;
    esp_zb_get_extended_pan_id(ext_pan);
    esp_zb_ieee_addr_t coord_ieee;
    esp_zb_get_long_address(coord_ieee);

    uint8_t online = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = dm_get_by_index(i);
        if (d && d->online) online++;
    }

    printf("[T+%07.3f] NETWORK channel=%u pan_id=0x%04X "
           "devices=%u/%u online=%u permit_join=%s\n",
           utils_uptime_s(), channel, pan_id,
           dm_count(), MAX_DEVICES, online,
           button_handler_permit_join_active() ? "open" : "closed");

    printf("[T+%07.3f] COORD_IEEE=", utils_uptime_s());
    for (int i = 7; i >= 0; i--) printf("%02X", coord_ieee[i]);
    printf("\n");
}

static void cmd_task_list(void)
{
#if configUSE_TRACE_FACILITY == 1 && configGENERATE_RUN_TIME_STATS == 1
    static char task_buf[1024];
    vTaskList(task_buf);
    printf("[T+%07.3f] TASKS:\nName             State  Pri  Stack  Num\n%s\n",
           utils_uptime_s(), task_buf);
#else
    // Minimal task info without trace facility
    printf("[T+%07.3f] TASKS (trace not enabled — showing known tasks):\n"
           "  zigbee_main  led_task  serial_cmd_task  btn_task\n",
           utils_uptime_s());
#endif
}

static void cmd_heap_stats(void)
{
    size_t free_heap  = esp_get_free_heap_size();
    size_t min_free   = esp_get_minimum_free_heap_size();
    printf("[T+%07.3f] HEAP free=%u min_free=%u\n",
           utils_uptime_s(), (unsigned)free_heap, (unsigned)min_free);
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

    // Find slot index and save immediately
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
    di_enqueue(dev);
    printf("Re-interview queued for %s\n", dm_display_name(dev));
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
    printf(
        "[T+%07.3f] KEY MAP:\n"
        "  1 — JSON device list\n"
        "  2 — Network statistics\n"
        "  3 — FreeRTOS task list\n"
        "  4 — Heap statistics\n"
        "  5 — Interview queue status\n"
        "  n — Set friendly name\n"
        "  j — Toggle permit join\n"
        "  r — Re-interview device\n"
        "  e — Erase NVS device cache\n"
        "  ? — This help\n",
        utils_uptime_s());
}

// ---------------------------------------------------------------------------
// Main task
// ---------------------------------------------------------------------------

static void serial_cmd_task(void *arg)
{
    (void)arg;
    uint8_t c;

    printf("[T+%07.3f] Serial command task ready. Press '?' for help.\n",
           utils_uptime_s());

    for (;;) {
        // Read one byte; short timeout keeps the task from being hung forever
        int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        switch ((char)c) {
            case '1': cmd_device_list();      break;
            case '2': cmd_network_stats();    break;
            case '3': cmd_task_list();        break;
            case '4': cmd_heap_stats();       break;
            case '5':
                printf("[T+%07.3f] Interview: use logs to track state.\n",
                       utils_uptime_s());
                break;
            case 'n': cmd_set_friendly_name(); break;
            case 'j':
                // Toggle permit join via button handler logic
                if (!button_handler_permit_join_active()) {
                    // Simulate button press — call esp_zb_bdb_open_network directly
                    esp_zb_bdb_open_network(180);
                    ZB_LOG("PERMIT_JOIN OPEN via serial (180s)");
                } else {
                    esp_zb_bdb_close_network();
                    ZB_LOG("PERMIT_JOIN CLOSED via serial");
                }
                break;
            case 'r': cmd_reinterview();      break;
            case 'e': cmd_erase_cache();      break;
            case '?': cmd_help();             break;
            case '\r': case '\n':             break;  // ignore newlines
            default:
                printf("[T+%07.3f] Unknown key '\\x%02X'. Press '?' for help.\n",
                       utils_uptime_s(), c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void serial_cmd_init(void)
{
    // UART0 is already configured by ESP-IDF for console.
    // We just install the driver to use uart_read_bytes.
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    xTaskCreate(serial_cmd_task, "serial_cmd_task", 4096, NULL, 3, NULL);
    ZB_LOG("Serial command handler init OK");
}
