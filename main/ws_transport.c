#include "ws_transport.h"

#include "app_config.h"
#include "button_handler.h"
#include "client_actions.h"
#include "device_manager.h"
#include "eth_driver.h"
#include "nvs_cache.h"
#include "tcp_console.h"
#include "time_sync.h"
#include "utils.h"
#include "ws_client_session.h"
#include "ws_model.h"
#include "ws_protocol.h"
#include "zcl_handler.h"
#include "zigbee_core.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WS_URI             "/ws"
#define WS_HTTP_PORT       8080
#define WS_TX_QUEUE_LEN    16
#define WS_TASK_STACK      6144
#define WS_TASK_PRIORITY   4
#define WS_RX_MAX_MESSAGE  1024
#define WS_REFRESH_DELAY_MS 200
#define WS_SEND_WAIT_TIMEOUT_S 1
#define WS_RECV_WAIT_TIMEOUT_S 1
#define WS_TX_LATENCY_WARN_MS  250
#define WS_SEND_TIME_WARN_MS   100
#define WS_HTTPD_STACK_SIZE    8192
#define WS_TX_LOCK_WAIT_CRITICAL_MS 50
#define WS_TX_LOCK_WAIT_STRUCTURAL_MS 10

#define WS_NOTIFY_AUTONOMOUS_SYNC BIT0
#define WS_NOTIFY_INVENTORY_REFRESH BIT1
#define WEB_API_BODY_MAX 1024
#define WEB_ZB_LOCK_WAIT_MS 100
#define WEB_EVENT_HISTORY_LEN 24
#define WEB_JSON_CHUNK_SIZE 768

typedef enum {
    WS_TX_PRIORITY_TELEMETRY = 0,
    WS_TX_PRIORITY_STRUCTURAL = 1,
    WS_TX_PRIORITY_CRITICAL = 2,
} ws_tx_priority_t;

typedef struct {
    bool in_use;
    ws_tx_priority_t priority;
    TickType_t enqueued_tick;
    uint32_t seq;
    char payload[WS_PROTOCOL_MAX_MESSAGE];
} ws_tx_msg_t;

typedef struct {
    uint32_t enqueued;
    uint32_t sent;
    uint32_t queue_full;
    uint32_t dropped_telemetry;
    uint32_t dropped_structural;
    uint32_t dropped_critical;
    uint32_t evicted_telemetry;
    uint32_t evicted_structural;
    uint32_t send_failures;
    uint32_t reconnects;
    uint8_t high_water;
} ws_tx_metrics_t;

static ws_tx_msg_t s_tx_queue[WS_TX_QUEUE_LEN];
static SemaphoreHandle_t s_tx_lock;
static uint8_t s_tx_count;
static uint32_t s_tx_seq;
static ws_tx_metrics_t s_metrics;
static EventGroupHandle_t s_eth_eg;
static httpd_handle_t s_server;
static TaskHandle_t s_task_handle;
static portMUX_TYPE s_refresh_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inventory_refresh_pending;
static TickType_t s_inventory_refresh_due_tick;

typedef struct {
    bool in_use;
    uint32_t ts_s;
    zb_evt_type_t type;
    uint64_t ieee;
    char friendly_name[ZB_EVT_NAME_LEN];
    bool online;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t permit_join_duration;
    char status[16];
} web_event_entry_t;

static web_event_entry_t s_web_events[WEB_EVENT_HISTORY_LEN];
static uint32_t s_web_event_seq;
static portMUX_TYPE s_web_event_lock = portMUX_INITIALIZER_UNLOCKED;

static void clear_inventory_refresh(void)
{
    portENTER_CRITICAL(&s_refresh_lock);
    s_inventory_refresh_pending = false;
    s_inventory_refresh_due_tick = 0;
    portEXIT_CRITICAL(&s_refresh_lock);
}

static const char *web_event_type_name(zb_evt_type_t type)
{
    switch (type) {
        case ZB_EVT_DEVICE_JOINED:
            return "device_joined";
        case ZB_EVT_DEVICE_LEAVE:
            return "device_left";
        case ZB_EVT_DEVICE_UPDATED:
            return "device_updated";
        case ZB_EVT_INTERVIEW:
            return "interview";
        case ZB_EVT_ATTR_CHANGED:
            return "attribute";
        case ZB_EVT_AVAILABILITY:
            return "availability";
        case ZB_EVT_PERMIT_JOIN:
            return "permit_join";
        default:
            return "event";
    }
}

static void web_events_record_zigbee(const zb_event_t *evt)
{
    if (!evt) {
        return;
    }

    web_event_entry_t entry = {
        .in_use = true,
        .ts_s = utils_uptime_ms() / 1000u,
        .type = evt->type,
        .ieee = evt->ieee,
        .online = evt->online,
        .endpoint = evt->endpoint,
        .cluster_id = evt->cluster_id,
        .attr_id = evt->attr_id,
        .permit_join_duration = evt->permit_join_duration,
    };
    strncpy(entry.friendly_name, evt->friendly_name,
            sizeof(entry.friendly_name) - 1);
    if (evt->interview_status) {
        strncpy(entry.status, evt->interview_status, sizeof(entry.status) - 1);
    }

    portENTER_CRITICAL(&s_web_event_lock);
    s_web_events[s_web_event_seq % WEB_EVENT_HISTORY_LEN] = entry;
    s_web_event_seq++;
    portEXIT_CRITICAL(&s_web_event_lock);
}

static void web_events_record_action(const char *status, uint64_t ieee,
                                     const char *friendly_name)
{
    web_event_entry_t entry = {
        .in_use = true,
        .ts_s = utils_uptime_ms() / 1000u,
        .type = ZB_EVT_DEVICE_UPDATED,
        .ieee = ieee,
    };
    if (status) {
        strncpy(entry.status, status, sizeof(entry.status) - 1);
    }
    if (friendly_name) {
        strncpy(entry.friendly_name, friendly_name,
                sizeof(entry.friendly_name) - 1);
    }

    portENTER_CRITICAL(&s_web_event_lock);
    s_web_events[s_web_event_seq % WEB_EVENT_HISTORY_LEN] = entry;
    s_web_event_seq++;
    portEXIT_CRITICAL(&s_web_event_lock);
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        case ESP_RST_UNKNOWN:
        default:
            return "unknown";
    }
}

static const char *tx_priority_name(ws_tx_priority_t priority)
{
    switch (priority) {
        case WS_TX_PRIORITY_CRITICAL:
            return "critical";
        case WS_TX_PRIORITY_STRUCTURAL:
            return "structural";
        case WS_TX_PRIORITY_TELEMETRY:
        default:
            return "telemetry";
    }
}

static void tx_record_drop_locked(ws_tx_priority_t priority)
{
    switch (priority) {
        case WS_TX_PRIORITY_CRITICAL:
            s_metrics.dropped_critical++;
            break;
        case WS_TX_PRIORITY_STRUCTURAL:
            s_metrics.dropped_structural++;
            break;
        case WS_TX_PRIORITY_TELEMETRY:
        default:
            s_metrics.dropped_telemetry++;
            break;
    }
}

static void tx_record_drop_unlocked(ws_tx_priority_t priority)
{
    switch (priority) {
        case WS_TX_PRIORITY_CRITICAL:
            s_metrics.dropped_critical++;
            break;
        case WS_TX_PRIORITY_STRUCTURAL:
            s_metrics.dropped_structural++;
            break;
        case WS_TX_PRIORITY_TELEMETRY:
        default:
            s_metrics.dropped_telemetry++;
            break;
    }
}

static void log_tx_metrics(const char *reason)
{
    ZB_LOG("WS TX metrics reason=%s queued=%u high_water=%u enq=%lu sent=%lu full=%lu drops t/s/c=%lu/%lu/%lu evict t/s=%lu/%lu send_fail=%lu reconnects=%lu",
           reason ? reason : "?",
           (unsigned)s_tx_count,
           (unsigned)s_metrics.high_water,
           (unsigned long)s_metrics.enqueued,
           (unsigned long)s_metrics.sent,
           (unsigned long)s_metrics.queue_full,
           (unsigned long)s_metrics.dropped_telemetry,
           (unsigned long)s_metrics.dropped_structural,
           (unsigned long)s_metrics.dropped_critical,
           (unsigned long)s_metrics.evicted_telemetry,
           (unsigned long)s_metrics.evicted_structural,
           (unsigned long)s_metrics.send_failures,
           (unsigned long)s_metrics.reconnects);
}

static const char *payload_type(const char *payload)
{
    static char type[24];
    const char *type_pos = payload ? strstr(payload, "\"type\":\"") : NULL;

    snprintf(type, sizeof(type), "?");
    if (!type_pos) {
        return type;
    }

    type_pos += strlen("\"type\":\"");
    const char *type_end = strchr(type_pos, '"');
    if (!type_end) {
        return type;
    }

    size_t len = (size_t)(type_end - type_pos);
    if (len >= sizeof(type)) {
        len = sizeof(type) - 1u;
    }
    memcpy(type, type_pos, len);
    type[len] = '\0';
    return type;
}

static void purge_tx_queue(void)
{
    if (!s_tx_lock || xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    if (s_tx_count > 0) {
        ZB_LOG("WS TX queue purge dropped=%u", (unsigned)s_tx_count);
    }
    memset(s_tx_queue, 0, sizeof(s_tx_queue));
    s_tx_count = 0;
    xSemaphoreGive(s_tx_lock);
}

static bool send_payload_now(const char *payload, void *ctx)
{
    (void)ctx;

    if (!payload || payload[0] == '\0') {
        return false;
    }

    ws_client_session_snapshot_t session;
    ws_client_session_snapshot(&session);
    if (!session.active || !session.server) {
        return false;
    }

    if (httpd_ws_get_fd_info(session.server, session.sockfd) !=
        HTTPD_WS_CLIENT_WEBSOCKET) {
        if (ws_client_session_close(session.sockfd)) {
            purge_tx_queue();
        }
        return false;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };

    int64_t send_start_us = esp_timer_get_time();
    esp_err_t err = httpd_ws_send_data(session.server, session.sockfd, &frame);
    uint32_t send_ms =
        (uint32_t)((esp_timer_get_time() - send_start_us) / 1000);
    if (send_ms >= WS_SEND_TIME_WARN_MS) {
        ZB_LOG("WS TX slow send type=%s bytes=%u duration_ms=%lu",
               payload_type(payload), (unsigned)frame.len,
               (unsigned long)send_ms);
    }

    if (err != ESP_OK) {
        ZB_LOG("WS TX failed err=0x%X, closing session", err);
        httpd_sess_trigger_close(session.server, session.sockfd);
        if (ws_client_session_close(session.sockfd)) {
            purge_tx_queue();
        }
        return false;
    }

    return true;
}

static int tx_find_free_locked(void)
{
    for (int i = 0; i < WS_TX_QUEUE_LEN; i++) {
        if (!s_tx_queue[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int tx_find_evictable_locked(ws_tx_priority_t incoming)
{
    ws_tx_priority_t target_priority;
    uint32_t oldest_seq = UINT32_MAX;
    int oldest_idx = -1;

    if (incoming == WS_TX_PRIORITY_TELEMETRY) {
        return -1;
    }

    target_priority = WS_TX_PRIORITY_TELEMETRY;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < WS_TX_QUEUE_LEN; i++) {
            if (!s_tx_queue[i].in_use ||
                s_tx_queue[i].priority != target_priority) {
                continue;
            }
            if (s_tx_queue[i].seq < oldest_seq) {
                oldest_seq = s_tx_queue[i].seq;
                oldest_idx = i;
            }
        }
        if (oldest_idx >= 0 || incoming != WS_TX_PRIORITY_CRITICAL) {
            return oldest_idx;
        }
        target_priority = WS_TX_PRIORITY_STRUCTURAL;
        oldest_seq = UINT32_MAX;
    }

    return oldest_idx;
}

static void tx_store_locked(int idx, const char *payload, size_t len,
                            ws_tx_priority_t priority)
{
    s_tx_queue[idx].in_use = true;
    s_tx_queue[idx].priority = priority;
    s_tx_queue[idx].enqueued_tick = xTaskGetTickCount();
    s_tx_queue[idx].seq = ++s_tx_seq;
    memcpy(s_tx_queue[idx].payload, payload, len + 1u);
    if (s_tx_count < WS_TX_QUEUE_LEN) {
        s_tx_count++;
    }
    if (s_tx_count > s_metrics.high_water) {
        s_metrics.high_water = s_tx_count;
    }
    s_metrics.enqueued++;
}

/*
 * TX backpressure policy:
 * - The queue is fixed at WS_TX_QUEUE_LEN messages; it never allocates or grows.
 * - Critical messages are session/control/errors/command results.
 * - Structural messages are device topology and stream-affecting events.
 * - Telemetry messages are attribute/availability updates and may be superseded.
 * - When full, telemetry is dropped. Structural may evict oldest telemetry.
 *   Critical may evict oldest telemetry, then oldest structural. Critical is
 *   dropped only if the queue is already full of critical messages.
 */
static bool enqueue_payload_prio(const char *payload, ws_tx_priority_t priority)
{
    if (!payload || payload[0] == '\0') {
        return false;
    }

    size_t len = strnlen(payload, WS_PROTOCOL_MAX_MESSAGE);
    if (len >= WS_PROTOCOL_MAX_MESSAGE) {
        ZB_LOG("WS TX drop oversized payload priority=%s",
               tx_priority_name(priority));
        return false;
    }

    TickType_t lock_wait = 0;
    if (priority == WS_TX_PRIORITY_CRITICAL) {
        lock_wait = pdMS_TO_TICKS(WS_TX_LOCK_WAIT_CRITICAL_MS);
    } else if (priority == WS_TX_PRIORITY_STRUCTURAL) {
        lock_wait = pdMS_TO_TICKS(WS_TX_LOCK_WAIT_STRUCTURAL_MS);
    }

    if (!s_tx_lock || xSemaphoreTake(s_tx_lock, lock_wait) != pdTRUE) {
        tx_record_drop_unlocked(priority);
        ZB_LOG("WS TX queue busy after %lu ms, drop priority=%s type=%s",
               (unsigned long)pdTICKS_TO_MS(lock_wait),
               tx_priority_name(priority), payload_type(payload));
        return false;
    }

    int idx = tx_find_free_locked();
    if (idx < 0) {
        s_metrics.queue_full++;
        int evict_idx = tx_find_evictable_locked(priority);
        if (evict_idx >= 0) {
            ws_tx_priority_t evicted_priority = s_tx_queue[evict_idx].priority;
            if (evicted_priority == WS_TX_PRIORITY_TELEMETRY) {
                s_metrics.evicted_telemetry++;
            } else if (evicted_priority == WS_TX_PRIORITY_STRUCTURAL) {
                s_metrics.evicted_structural++;
            }
            ZB_LOG("WS TX queue full: evict priority=%s type=%s for priority=%s",
                   tx_priority_name(evicted_priority),
                   payload_type(s_tx_queue[evict_idx].payload),
                   tx_priority_name(priority));
            memset(&s_tx_queue[evict_idx], 0, sizeof(s_tx_queue[evict_idx]));
            s_tx_count--;
            idx = evict_idx;
        } else {
            tx_record_drop_locked(priority);
            ZB_LOG("WS TX queue full: drop priority=%s type=%s drops t/s/c=%lu/%lu/%lu",
                   tx_priority_name(priority), payload_type(payload),
                   (unsigned long)s_metrics.dropped_telemetry,
                   (unsigned long)s_metrics.dropped_structural,
                   (unsigned long)s_metrics.dropped_critical);
            xSemaphoreGive(s_tx_lock);
            return false;
        }
    }

    tx_store_locked(idx, payload, len, priority);
    xSemaphoreGive(s_tx_lock);
    return true;
}

static bool enqueue_payload_critical(const char *payload, void *ctx)
{
    (void)ctx;
    return enqueue_payload_prio(payload, WS_TX_PRIORITY_CRITICAL);
}

static bool enqueue_payload_structural(const char *payload, void *ctx)
{
    (void)ctx;
    return enqueue_payload_prio(payload, WS_TX_PRIORITY_STRUCTURAL);
}

static bool enqueue_payload_telemetry(const char *payload, void *ctx)
{
    (void)ctx;
    return enqueue_payload_prio(payload, WS_TX_PRIORITY_TELEMETRY);
}

static ws_tx_priority_t payload_priority(const char *payload)
{
    const char *type = payload_type(payload);

    if (strcmp(type, "hello_ack") == 0 ||
        strcmp(type, "ack") == 0 ||
        strcmp(type, "pong") == 0 ||
        strcmp(type, "cmd_result") == 0 ||
        strcmp(type, "error") == 0) {
        return WS_TX_PRIORITY_CRITICAL;
    }

    if (strcmp(type, "inventory_chunk") == 0 ||
        strcmp(type, "state_chunk") == 0 ||
        strcmp(type, "device_joined") == 0 ||
        strcmp(type, "device_left") == 0 ||
        strcmp(type, "device_updated") == 0) {
        return WS_TX_PRIORITY_STRUCTURAL;
    }

    return WS_TX_PRIORITY_TELEMETRY;
}

static bool enqueue_payload_auto(const char *payload, void *ctx)
{
    (void)ctx;
    return enqueue_payload_prio(payload, payload_priority(payload));
}

static bool tx_pop_next(ws_tx_msg_t *out)
{
    int best_idx = -1;
    ws_tx_priority_t best_priority = WS_TX_PRIORITY_TELEMETRY;
    uint32_t best_seq = UINT32_MAX;

    if (!out || !s_tx_lock ||
        xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < WS_TX_QUEUE_LEN; i++) {
        if (!s_tx_queue[i].in_use) {
            continue;
        }
        if (best_idx < 0 ||
            s_tx_queue[i].priority > best_priority ||
            (s_tx_queue[i].priority == best_priority &&
             s_tx_queue[i].seq < best_seq)) {
            best_idx = i;
            best_priority = s_tx_queue[i].priority;
            best_seq = s_tx_queue[i].seq;
        }
    }

    if (best_idx < 0) {
        xSemaphoreGive(s_tx_lock);
        return false;
    }

    *out = s_tx_queue[best_idx];
    memset(&s_tx_queue[best_idx], 0, sizeof(s_tx_queue[best_idx]));
    if (s_tx_count > 0) {
        s_tx_count--;
    }
    xSemaphoreGive(s_tx_lock);
    return true;
}

static bool schedule_inventory_refresh(void)
{
    if (!ws_client_session_is_active() || !s_task_handle) {
        return false;
    }

    portENTER_CRITICAL(&s_refresh_lock);
    bool already_pending = s_inventory_refresh_pending;
    s_inventory_refresh_pending = true;
    s_inventory_refresh_due_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(WS_REFRESH_DELAY_MS);
    portEXIT_CRITICAL(&s_refresh_lock);

    ZB_LOG("WS inventory refresh %s due_in_ms=%u",
           already_pending ? "coalesced" : "scheduled",
           (unsigned)WS_REFRESH_DELAY_MS);
    xTaskNotify(s_task_handle, WS_NOTIFY_INVENTORY_REFRESH, eSetBits);
    return true;
}

static bool take_inventory_refresh_if_due(void)
{
    TickType_t now = xTaskGetTickCount();
    bool due = false;

    portENTER_CRITICAL(&s_refresh_lock);
    if (s_inventory_refresh_pending &&
        (int32_t)(now - s_inventory_refresh_due_tick) >= 0) {
        s_inventory_refresh_pending = false;
        s_inventory_refresh_due_tick = 0;
        due = true;
    }
    portEXIT_CRITICAL(&s_refresh_lock);

    return due;
}

static void send_inventory_refresh_now(void)
{
    if (!ws_client_session_is_active()) {
        clear_inventory_refresh();
        return;
    }

    ZB_LOG("WS inventory refresh: sending fragmented inventory");
    bool ok = ws_protocol_send_inventory_stream(send_payload_now, NULL);

    ZB_LOG("WS inventory refresh: sending state snapshot");
    ok = ws_protocol_send_state_stream(send_payload_now, NULL) && ok;

    ZB_LOG("WS inventory refresh: %s", ok ? "complete" : "failed");
}

static void drain_tx_queue(void)
{
    ws_tx_msg_t msg;

    while (tx_pop_next(&msg)) {
        uint32_t queue_ms =
            (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - msg.enqueued_tick);
        if (queue_ms >= WS_TX_LATENCY_WARN_MS) {
            ZB_LOG("WS TX queue latency priority=%s type=%s latency_ms=%lu",
                   tx_priority_name(msg.priority), payload_type(msg.payload),
                   (unsigned long)queue_ms);
        }

        if (send_payload_now(msg.payload, NULL)) {
            s_metrics.sent++;
        } else {
            s_metrics.send_failures++;
            log_tx_metrics("send_failed");
        }
    }
}

static void start_autonomous_debug_stream(void)
{
    if (!ws_client_session_is_active()) {
        return;
    }

    ZB_LOG("WS autonomous sync: begin (no client hello required)");
    bool ok = ws_protocol_send_bootstrap(send_payload_now, NULL);
    ZB_LOG("WS autonomous sync: %s", ok ? "complete" : "failed");
    if (ok) {
        ZB_LOG("WS stream mode: active, forwarding future Zigbee events");
    }
}

static esp_err_t ws_recv_text(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
    };

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int sockfd = httpd_req_to_sockfd(req);
        if (ws_client_session_close(sockfd)) {
            purge_tx_queue();
            clear_inventory_refresh();
            ZB_LOG("WS client close frame fd=%d", sockfd);
            log_tx_metrics("close_frame");
        }
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    if (frame.len >= WS_RX_MAX_MESSAGE) {
        ZB_LOG("WS RX drop oversized frame len=%u", (unsigned)frame.len);
        ws_protocol_send_error(enqueue_payload_critical, NULL, 0, "malformed_message",
                               "WebSocket frame exceeds input limit");
        return ESP_OK;
    }

    char *buf = calloc(1u, frame.len + 1u);
    if (!buf) {
        ws_protocol_send_error(enqueue_payload_critical, NULL, 0,
                               "internal_error",
                               "Unable to allocate receive buffer");
        return ESP_ERR_NO_MEM;
    }

    frame.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    buf[frame.len] = '\0';
    ZB_LOG("WS RX text frame bytes=%u", (unsigned)frame.len);
    ws_protocol_handle_text(buf, enqueue_payload_auto, NULL);
    free(buf);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ws_client_session_snapshot_t previous;
        ws_client_session_snapshot(&previous);
        if (previous.active && previous.server && previous.sockfd != sockfd) {
            httpd_sess_trigger_close(previous.server, previous.sockfd);
            ws_client_session_close(previous.sockfd);
            s_metrics.reconnects++;
        }

        purge_tx_queue();
        clear_inventory_refresh();
        ws_client_session_open(s_server, sockfd);
        ZB_LOG("WS client connected fd=%d mode=autonomous_debug_stream reconnects=%lu",
               sockfd, (unsigned long)s_metrics.reconnects);
        log_tx_metrics("connect");
        ZB_LOG("WS autonomous sync: scheduled after handshake");
        xTaskNotify(s_task_handle, WS_NOTIFY_AUTONOMOUS_SYNC, eSetBits);
        return ESP_OK;
    }

    if (!ws_client_session_matches(sockfd)) {
        return ESP_OK;
    }
    return ws_recv_text(req);
}

static void ws_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    if (ws_client_session_close(sockfd)) {
        purge_tx_queue();
        clear_inventory_refresh();
        ZB_LOG("WS client disconnected fd=%d", sockfd);
        log_tx_metrics("disconnect");
    }
    close(sockfd);
}

static const char s_web_index_html[] =
"<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESP32 Zigbee</title><style>"
":root{color-scheme:light dark;font-family:system-ui,-apple-system,Segoe UI,sans-serif}"
"body{margin:0;background:#f5f7f9;color:#172026}"
"header{background:#172026;color:#fff;padding:18px 22px}"
"nav{background:#24313b;padding:0 22px;display:flex;gap:4px;flex-wrap:wrap}nav a{color:#edf2f6;text-decoration:none;padding:10px 12px;border-bottom:3px solid transparent}nav a.active{border-color:#69aee8;background:#172026}"
"main{max-width:1120px;margin:0 auto;padding:18px;display:grid;gap:16px}"
"section{background:#fff;border:1px solid #d8dee4;border-radius:8px;padding:16px}.page{display:none}.page.active{display:block}"
"h1{font-size:22px;margin:0}h2{font-size:17px;margin:0 0 12px}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}"
".metric{border:1px solid #e4e8ec;border-radius:6px;padding:10px;background:#fbfcfd}"
".panel{margin-top:12px;border-top:1px solid #e6ebef;padding-top:12px;display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px}.panel strong{display:block;margin-top:2px}"
".stack{display:grid;gap:12px}.deviceTitle{display:flex;justify-content:space-between;gap:10px;align-items:flex-start;flex-wrap:wrap}.pill{display:inline-block;border-radius:999px;padding:3px 8px;background:#eaf1f7;color:#394650;font-size:12px;margin:2px 4px 2px 0}"
".label{font-size:12px;color:#66727c}.value{font-size:20px;font-weight:650;margin-top:4px}"
"label{display:grid;gap:5px;font-size:13px;color:#394650}input{font:inherit;padding:9px;border:1px solid #c8d0d8;border-radius:6px}"
"button{font:inherit;border:0;border-radius:6px;padding:9px 12px;background:#145c9e;color:#fff;cursor:pointer}"
"a{color:#145c9e}a.small{display:inline-block;text-decoration:none;border-radius:6px;background:#145c9e;color:#fff}button.secondary,a.secondary{background:#596775}button.warn{background:#a33b21}.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}"
"table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:left;padding:8px;border-bottom:1px solid #e6ebef}"
".rename{display:flex;gap:6px;align-items:center}.rename input{min-width:120px;max-width:190px;padding:7px}.small{padding:7px 9px;font-size:12px}"
".toolbar{display:flex;gap:8px;align-items:end;flex-wrap:wrap;margin-bottom:10px}.toolbar label{min-width:180px}select{font:inherit;padding:9px;border:1px solid #c8d0d8;border-radius:6px;background:#fff}.detail{margin-top:8px;color:#596775}.events{display:grid;gap:6px}.event{display:grid;grid-template-columns:80px 130px 1fr;gap:8px;border-bottom:1px solid #e6ebef;padding:7px 0;font-size:13px}"
".ok{color:#167145}.bad{color:#a33b21}.msg{min-height:20px;color:#596775;font-size:13px}"
"@media (prefers-color-scheme:dark){body{background:#11171c;color:#edf2f6}section{background:#172026;border-color:#2a3540}.metric{background:#1d2730;border-color:#2f3a45}.panel{border-color:#2a3540}.pill{background:#24313b;color:#d8e2ea}.label{color:#aeb9c2}input,select{background:#11171c;color:#edf2f6;border-color:#3b4854}th,td,.event{border-color:#2a3540}.detail{color:#aeb9c2}}"
"</style></head><body><header><h1>ESP32 Zigbee Coordinator</h1></header>"
"<nav><a href=\"/\" data-page=\"status\">Estado</a><a href=\"/devices\" data-page=\"devices\">Dispositivos</a><a href=\"/zigbee\" data-page=\"zigbee\">Zigbee</a><a href=\"/network\" data-page=\"network\">Red</a><a href=\"/events\" data-page=\"events\">Eventos</a><a href=\"/config\" data-page=\"config\">Configuracion</a><a href=\"/actions\" data-page=\"actions\">Acciones</a></nav><main>"
"<section class=\"page\" id=\"page-status\"><h2>Estado</h2><div class=\"actions\"><button class=\"secondary\" id=\"autoRefresh\">Pausar auto-refresh</button><button class=\"secondary\" id=\"exportJson\">Exportar JSON</button></div><div class=\"grid\" id=\"metrics\"></div><h2>Diagnostico</h2><div class=\"panel\" id=\"systemDiag\"></div></section>"
"<section class=\"page\" id=\"page-config\"><h2>Configuracion</h2><div class=\"grid\">"
"<label>Nombre mDNS<input id=\"mdns_hostname\" maxlength=\"31\"></label>"
"<label>Nombre visible<input id=\"mdns_instance\" maxlength=\"63\"></label>"
"<label>Servidor NTP<input id=\"ntp_server\" maxlength=\"63\"></label>"
"<label>Zona horaria POSIX<select id=\"timezone\"></select></label>"
"<div class=\"metric\"><div class=\"label\">Fecha/hora actual</div><div class=\"value\" id=\"current_time\">-</div><div class=\"detail\" id=\"current_time_detail\">-</div></div>"
"<label>Join por defecto (s)<input id=\"permit_join_duration_s\" type=\"number\" min=\"10\" max=\"254\"></label>"
"</div><h2>Reporting</h2><div class=\"grid\">"
"<label>Max always-on (s)<input id=\"report_always_on_max_s\" type=\"number\" min=\"30\" max=\"3600\"></label>"
"<label>Max sleepy (s)<input id=\"report_sleepy_max_s\" type=\"number\" min=\"300\" max=\"43200\"></label>"
"<label>Margen presencia (s)<input id=\"presence_grace_s\" type=\"number\" min=\"5\" max=\"3600\"></label>"
"</div><div class=\"actions\"><button id=\"save\">Guardar</button><button class=\"secondary\" id=\"refresh\">Actualizar</button></div><div class=\"msg\" id=\"msg\"></div></section>"
"<section class=\"page\" id=\"page-actions\"><h2>Join</h2><div class=\"actions\"><button id=\"joinOpen\">Abrir join</button><button class=\"warn\" id=\"joinClose\">Cerrar join</button></div><h2>Acciones</h2><div class=\"actions\"><button class=\"secondary\" id=\"timeResync\">Sincronizar hora</button><button class=\"secondary\" id=\"configureAll\">Reconfigurar reporting en todos</button><button class=\"secondary\" id=\"closeWs\">Cerrar WebSocket</button><button class=\"warn\" id=\"eraseCache\">Borrar cache dispositivos</button><button class=\"warn\" id=\"reboot\">Reiniciar</button><button class=\"warn\" id=\"zbReset\">Reset red Zigbee</button></div></section>"
"<section class=\"page\" id=\"page-devices\"><h2>Dispositivos</h2><div class=\"toolbar\"><label>Buscar<input id=\"deviceSearch\" placeholder=\"nombre, IEEE o modelo\"></label><label>Filtro<select id=\"deviceFilter\"><option value=\"all\">Todos</option><option value=\"online\">Online</option><option value=\"offline\">Offline</option><option value=\"sleepy\">Sleepy</option><option value=\"router\">Routers / always-on</option><option value=\"reporting\">Reporting pendiente</option></select></label></div><table><thead><tr><th>Nombre</th><th>IEEE</th><th>Estado</th><th>Lecturas</th><th>Modelo</th><th>Acciones</th></tr></thead><tbody id=\"devices\"></tbody></table></section>"
"<section class=\"page\" id=\"page-device\"><div id=\"devicePage\" class=\"stack\"></div></section>"
"<section class=\"page\" id=\"page-zigbee\"><h2>Salud Zigbee</h2><div class=\"grid\" id=\"zigbeeMetrics\"></div><div class=\"panel\" id=\"zigbeeDiag\"></div><h2>Actividad Zigbee</h2><div class=\"events\" id=\"zigbeeEvents\"></div></section>"
"<section class=\"page\" id=\"page-network\"><h2>Red y servicios</h2><div class=\"grid\" id=\"networkMetrics\"></div><div class=\"panel\" id=\"networkDiag\"></div></section>"
"<section class=\"page\" id=\"page-events\"><h2>Eventos recientes</h2><div class=\"events\" id=\"events\"></div></section>"
"</main><script>"
"let cfg={},lastStatus=null,autoRefresh=true,configDirty=false;"
"const CONFIG_FIELD_IDS=['mdns_hostname','mdns_instance','ntp_server','timezone','permit_join_duration_s','report_always_on_max_s','report_sleepy_max_s','presence_grace_s'];"
"const TZ_OPTIONS=["
"{v:'UTC12',l:'UTC-12:00 - fija'},{v:'UTC11',l:'UTC-11:00 - fija'},{v:'HST10',l:'UTC-10:00 - Hawai (HST)'},{v:'UTC9:30',l:'UTC-09:30 - fija'},{v:'AKST9AKDT,M3.2.0,M11.1.0',l:'UTC-09:00 / -08:00 - Alaska'},{v:'PST8PDT,M3.2.0,M11.1.0',l:'UTC-08:00 / -07:00 - Pacifico'},{v:'MST7MDT,M3.2.0,M11.1.0',l:'UTC-07:00 / -06:00 - Montana'},{v:'MST7',l:'UTC-07:00 - Arizona/MST fija'},{v:'CST6CDT,M3.2.0,M11.1.0',l:'UTC-06:00 / -05:00 - Central US'},{v:'EST5EDT,M3.2.0,M11.1.0',l:'UTC-05:00 / -04:00 - Este US'},{v:'EST5',l:'UTC-05:00 - EST fija'},{v:'UTC4',l:'UTC-04:00 - fija'},{v:'UTC3:30',l:'UTC-03:30 - fija'},{v:'UTC3',l:'UTC-03:00 - fija'},{v:'UTC2',l:'UTC-02:00 - fija'},{v:'UTC1',l:'UTC-01:00 - fija'},"
"{v:'UTC0',l:'UTC+00:00 - UTC/GMT'},{v:'GMT0BST,M3.5.0/1,M10.5.0',l:'UTC+00:00 / +01:00 - Reino Unido'},{v:'WET0WEST,M3.5.0/1,M10.5.0',l:'UTC+00:00 / +01:00 - Europa oeste'},"
"{v:'UTC-1',l:'UTC+01:00 - fija'},{v:'CET-1CEST,M3.5.0,M10.5.0/3',l:'UTC+01:00 / +02:00 - Europa central'},{v:'UTC-2',l:'UTC+02:00 - fija'},{v:'EET-2EEST,M3.5.0/3,M10.5.0/4',l:'UTC+02:00 / +03:00 - Europa este'},{v:'UTC-3',l:'UTC+03:00 - fija'},{v:'UTC-3:30',l:'UTC+03:30 - fija'},{v:'UTC-4',l:'UTC+04:00 - fija'},{v:'UTC-4:30',l:'UTC+04:30 - fija'},{v:'UTC-5',l:'UTC+05:00 - fija'},{v:'UTC-5:30',l:'UTC+05:30 - India'},{v:'UTC-5:45',l:'UTC+05:45 - Nepal'},{v:'UTC-6',l:'UTC+06:00 - fija'},{v:'UTC-6:30',l:'UTC+06:30 - fija'},{v:'UTC-7',l:'UTC+07:00 - fija'},{v:'UTC-8',l:'UTC+08:00 - fija'},{v:'UTC-8:45',l:'UTC+08:45 - fija'},{v:'JST-9',l:'UTC+09:00 - Japon/Corea'},{v:'UTC-9:30',l:'UTC+09:30 - fija'},{v:'ACST-9:30ACDT,M10.1.0,M4.1.0/3',l:'UTC+09:30 / +10:30 - Australia central'},{v:'UTC-10',l:'UTC+10:00 - fija'},{v:'AEST-10AEDT,M10.1.0,M4.1.0/3',l:'UTC+10:00 / +11:00 - Australia este'},{v:'UTC-10:30',l:'UTC+10:30 - fija'},{v:'UTC-11',l:'UTC+11:00 - fija'},{v:'UTC-12',l:'UTC+12:00 - fija'},{v:'NZST-12NZDT,M9.5.0,M4.1.0/3',l:'UTC+12:00 / +13:00 - Nueva Zelanda'},{v:'UTC-12:45',l:'UTC+12:45 - fija'},{v:'UTC-13',l:'UTC+13:00 - fija'},{v:'UTC-14',l:'UTC+14:00 - fija'}];"
"async function j(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text()||r.status);return r.json();}"
"function esc(v){return String(v==null?'':v).replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]));}"
"function setMsg(t){document.getElementById('msg').textContent=t||'';}"
"function metric(label,value,cls){return `<div class=\"metric\"><div class=\"label\">${label}</div><div class=\"value ${cls||''}\">${value}</div></div>`}"
"function readings(d){let r=d.readings||{},a=[];for(const k of Object.keys(r)){let x=r[k];a.push(`${esc(k)}: ${esc(x.value)}${x.unit?' '+esc(x.unit):''}`)}return a.join('<br>')||'-'}"
"function reportingLabel(r){if(!r)return'pendiente';if(r.configured)return'ok';if(r.in_progress)return'configurando';if((r.failed||0)>0)return'fallo/parcial';return'pendiente'}"
"function reportingFailureRows(d){let rp=d.reporting||{},fs=rp.failures||[];if(!fs.length&&!rp.overflow)return '<div><span class=\"label\">Fallos reporting</span><strong>Sin fallos detallados</strong></div>';let rows=fs.map(f=>{let cname=esc(f.cluster_name||hex(f.cluster_id));let what=f.reason==='bind_fail'?('bind '+cname):(f.reason==='write_fail'?('write '+cname+' '+hex(f.attr_id)):('EP '+f.endpoint+' '+cname+' '+hex(f.attr_id)));let why=f.reason==='missing'?'sin respuesta':(f.reason==='bind_fail'?'bind status '+hex(f.status):(f.reason==='write_fail'?'write status '+hex(f.status):'status '+hex(f.status)));return `<div><span class=\"label\">${what}</span><strong>${why}</strong></div>`}).join('');if(rp.overflow)rows+='<div><span class=\"label\">Fallos reporting</span><strong>lista truncada</strong></div>';return rows}"
"function ago(ts){let d=Math.max(0,Math.floor((lastStatus.system.uptime_s||0)-ts));return d<60?d+'s':Math.floor(d/60)+'m'}"
"function bytes(n){if(n==null)return '-';return n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?(n/1024).toFixed(1)+' KB':n+' B'}"
"function hex(n){return n==null?'-':'0x'+Number(n).toString(16).toUpperCase()}"
"function clusters(a){return (a||[]).map(x=>'0x'+Number(x).toString(16).padStart(4,'0').toUpperCase()).join(', ')||'-'}"
"function detail(d){let eps=(d.endpoints||[]).map(e=>`EP ${e.id} ${esc(e.device_type)}<br>in: ${clusters(e.in_clusters)}<br>out: ${clusters(e.out_clusters)}`).join('<hr>')||'-';let st=d.stats||{};let rp=d.reporting||{};return `<details class=\"detail\"><summary>Detalle</summary><div>${eps}<hr>reporting ${rp.received||0}/${rp.expected||0}, fallos ${rp.failed||0}<br>reports ${st.report_attr_ok||0}, unchanged ${st.report_attr_unchanged||0}, read ok/fail ${st.read_rsp_ok||0}/${st.read_rsp_fail||0}</div></details>`}"
"function passFilter(d,q,f){let hay=(d.name+' '+d.ieee+' '+(d.model||'')+' '+(d.manufacturer||'')).toLowerCase();if(q&&hay.indexOf(q)<0)return false;if(f==='online')return d.online;if(f==='offline')return !d.online;if(f==='sleepy')return d.is_sleepy;if(f==='router')return !d.is_sleepy;if(f==='reporting')return !d.reporting||!d.reporting.configured||d.reporting.in_progress;return true}"
"function deviceById(id){id=String(id||'').toLowerCase();return (lastStatus&&lastStatus.devices||[]).find(d=>String(d.ieee).toLowerCase()===id)}"
"function editingDeviceName(){let el=document.activeElement;return el&&el.tagName==='INPUT'&&(el.id==='deviceName'||el.id.indexOf('name_')===0)}"
"function setupConfigDirty(){CONFIG_FIELD_IDS.forEach(id=>{let el=document.getElementById(id);if(!el)return;el.addEventListener('input',()=>configDirty=true);el.addEventListener('change',()=>configDirty=true)})}"
"function fillConfigForm(force){if(!cfg||(!force&&configDirty))return;document.getElementById('mdns_hostname').value=cfg.mdns_hostname;document.getElementById('mdns_instance').value=cfg.mdns_instance;document.getElementById('ntp_server').value=cfg.ntp_server;ensureTimezoneOption(cfg.timezone);document.getElementById('timezone').value=cfg.timezone;document.getElementById('permit_join_duration_s').value=cfg.permit_join_duration_s;document.getElementById('report_always_on_max_s').value=cfg.report_always_on_max_s;document.getElementById('report_sleepy_max_s').value=cfg.report_sleepy_max_s;document.getElementById('presence_grace_s').value=cfg.presence_grace_s;configDirty=false}"
"function attrRows(d){let r=d.readings||{};let rows=Object.keys(r).map(k=>{let x=r[k];return `<tr><td>${esc(k)}</td><td>${esc(x.value)}${x.unit?' '+esc(x.unit):''}</td><td>${x.endpoint||'-'}</td><td>${hex(x.cluster_id)}</td><td>${hex(x.attr_id)}</td><td>${ago(x.ts||0)}</td></tr>`}).join('');let raw=(d.attrs||[]).map(a=>`<tr><td>raw</td><td>${esc(a.raw)}</td><td>${a.endpoint}</td><td>${hex(a.cluster_id)}</td><td>${hex(a.attr_id)} / t ${hex(a.attr_type)}</td><td>${ago(a.ts||0)}</td></tr>`).join('');return rows+raw}"
"function endpointRows(d){return (d.endpoints||[]).map(e=>`<tr><td>${e.id}</td><td>${hex(e.profile_id)}</td><td>${hex(e.device_id)}<br>${esc(e.device_type)}</td><td>${clusters(e.in_clusters)}</td><td>${clusters(e.out_clusters)}</td></tr>`).join('')}"
"function renderDevices(){let s=lastStatus;if(!s||editingDeviceName())return;let q=document.getElementById('deviceSearch').value.toLowerCase();let f=document.getElementById('deviceFilter').value;let rows=s.devices.filter(d=>passFilter(d,q,f)).map((d,i)=>`<tr><td><a href=\"/device?id=${encodeURIComponent(d.ieee)}\">${esc(d.name)}</a>${detail(d)}</td><td>${esc(d.ieee)}</td><td class=\"${d.online?'ok':'bad'}\">${d.online?'online':'offline'} / ${esc(d.state)}<br>reporting: ${reportingLabel(d.reporting)}<br>${d.is_sleepy?'sleepy':'router'}</td><td>${readings(d)}</td><td>${esc(d.manufacturer||'-')} ${esc(d.model||'')}<br>${esc(d.power_source||'')}</td><td><div class=\"rename\"><input id=\"name_${i}\" value=\"${esc(d.name)}\"><button class=\"small\" onclick=\"renameDev('${d.ieee}',document.getElementById('name_${i}').value)\">Guardar</button></div><div class=\"actions\"><a class=\"small\" href=\"/device?id=${encodeURIComponent(d.ieee)}\">Abrir</a><button class=\"small secondary\" onclick=\"devAction('/api/device/reinterview','${d.ieee}')\">Re-entrevistar</button><button class=\"small secondary\" onclick=\"devAction('/api/device/configure','${d.ieee}')\">Reporting</button></div></td></tr>`).join('');document.getElementById('devices').innerHTML=rows||'<tr><td colspan=\"6\">Sin dispositivos</td></tr>'}"
"function renderEvents(){let s=lastStatus;if(!s)return;document.getElementById('events').innerHTML=(s.events||[]).map(e=>`<div class=\"event\"><span>${ago(e.ts)}</span><span>${esc(e.type)}</span><span>${esc(e.name||e.device_id||'sistema')} ${e.status?'- '+esc(e.status):''}${e.duration!=null?' ('+e.duration+'s)':''}</span></div>`).join('')||'<div class=\"msg\">Sin eventos recientes</div>'}"
"function renderSystem(){let s=lastStatus;if(!s)return;let y=s.system||{};let rows=[['Firmware',esc((y.app_name||'app')+' '+(y.app_version||''))],['IDF',esc(y.idf_version||'-')],['Build',esc((y.build_date||'-')+' '+(y.build_time||''))],['Reset',esc(y.reset_reason||'-')],['Heap libre',bytes(y.free_heap)],['Heap interno',bytes(y.free_internal_heap)],['Heap minimo',bytes(y.min_free_heap)],['Particion',esc(y.partition_label||'-')+' / '+bytes(y.partition_size)+' @ '+hex(y.partition_address)]];document.getElementById('systemDiag').innerHTML=rows.map(x=>`<div><span class=\"label\">${x[0]}</span><strong>${x[1]}</strong></div>`).join('')}"
"function panel(rows){return rows.map(x=>`<div><span class=\"label\">${x[0]}</span><strong>${x[1]}</strong></div>`).join('')}"
"function tzLabel(v){let o=TZ_OPTIONS.find(x=>x.v===v);return o?o.l:v}"
"function ensureTimezoneOption(v){let sel=document.getElementById('timezone');if(!v||[...sel.options].some(o=>o.value===v))return;let o=document.createElement('option');o.value=v;o.textContent='Guardada - '+v;sel.appendChild(o)}"
"function populateTimezones(){let sel=document.getElementById('timezone');sel.innerHTML='';TZ_OPTIONS.forEach(z=>{let o=document.createElement('option');o.value=z.v;o.textContent=z.l+' ['+z.v+']';sel.appendChild(o)})}"
"function renderConfigTime(){let t=lastStatus&&lastStatus.time||{};let cur=document.getElementById('current_time'),det=document.getElementById('current_time_detail');if(!cur||!det)return;cur.textContent=t.current_local||'Hora no sincronizada';cur.className='value '+(t.valid?'ok':'bad');det.textContent=t.valid?((t.utc_offset||'UTC?')+' | UTC '+(t.current_utc||'-')):'Esperando sincronizacion NTP'}"
"function renderNetwork(){let s=lastStatus;if(!s)return;let n=s.network||{},t=s.time||{},sv=s.services||{};document.getElementById('networkMetrics').innerHTML=[metric('Link',n.link_up?'activo':'caido',n.link_up?'ok':'bad'),metric('IP',n.has_ip?n.ip:'sin IP',n.has_ip?'ok':'bad'),metric('mDNS',cfg.mdns_hostname+'.local'),metric('NTP',t.synced?'sincronizado':'pendiente',t.synced?'ok':'bad'),metric('Hora',t.current_local||'sin sincronizar',t.valid?'ok':'bad'),metric('WebSocket',sv.websocket_client?'conectado':'sin cliente',sv.websocket_client?'ok':''),metric('TCP consola',sv.tcp_console_client?'conectada':'sin cliente',sv.tcp_console_client?'ok':'')].join('');document.getElementById('networkDiag').innerHTML=panel([['MAC',esc(n.mac||'-')],['IP',esc(n.ip||'-')],['Mascara',esc(n.netmask||'-')],['Gateway',esc(n.gateway||'-')],['mDNS',esc(cfg.mdns_hostname+'.local')],['Nombre visible',esc(cfg.mdns_instance||'-')],['Servidor NTP',esc(t.server||cfg.ntp_server||'-')],['Zona horaria',esc(tzLabel(t.timezone||cfg.timezone||'-'))],['Hora local',esc(t.current_local||'-')],['Hora UTC',esc(t.current_utc||'-')],['Offset UTC',esc(t.utc_offset||'-')],['Ultimo sync',t.last_sync_uptime_s?ago(t.last_sync_uptime_s):'-']])}"
"function renderZigbee(){let s=lastStatus;if(!s)return;let z=s.zigbee||{},p=s.permit_join||{},u=s.summary||{};document.getElementById('zigbeeMetrics').innerHTML=[metric('Estado',z.ready?'listo':'iniciando',z.ready?'ok':'bad'),metric('Canal',z.ready?z.channel:'-'),metric('PAN ID',z.ready?hex(z.pan_id):'-'),metric('Join',p.active?('abierto '+p.remaining_s+'s'):'cerrado',p.active?'ok':''),metric('Dispositivos',s.device_count+'/'+s.device_capacity),metric('Online',s.online_devices),metric('Sleepy/router',(u.sleepy_devices||0)+'/'+(u.router_devices||0)),metric('Pendientes',(u.reporting_pending||0)+' reporting / '+(u.interview_active||0)+' entrevista',u.reporting_pending||u.interview_active?'bad':'ok')].join('');document.getElementById('zigbeeDiag').innerHTML=panel([['Extended PAN',esc(z.ext_pan_id||'-')],['Coordinador IEEE',esc(z.coordinator_ieee||'-')],['Canal/PAN',z.ready?esc(z.channel+' / '+hex(z.pan_id)):'-'],['Permit join',p.active?esc('abierto '+p.remaining_s+'s'):'cerrado'],['Routers / sleepy',(u.router_devices||0)+' / '+(u.sleepy_devices||0)],['Reporting pendiente',String(u.reporting_pending||0)],['Entrevistas activas',String(u.interview_active||0)]]);document.getElementById('zigbeeEvents').innerHTML=(s.events||[]).filter(e=>e.type!=='device_updated'||e.device_id).slice(0,12).map(e=>`<div class=\"event\"><span>${ago(e.ts)}</span><span>${esc(e.type)}</span><span>${esc(e.name||e.device_id||'sistema')} ${e.status?'- '+esc(e.status):''}</span></div>`).join('')||'<div class=\"msg\">Sin eventos Zigbee recientes</div>'}"
"function renderDevicePage(){let s=lastStatus;if(!s||editingDeviceName())return;let id=new URLSearchParams(location.search).get('id');let box=document.getElementById('devicePage');if(!box)return;if(!id){box.innerHTML='<h2>Dispositivo</h2><div class=\"msg\">Selecciona un dispositivo desde la lista.</div>';return}let d=deviceById(id);if(!d){box.innerHTML='<h2>Dispositivo</h2><div class=\"msg\">No encontrado: '+esc(id)+'</div>';return}let rp=d.reporting||{},st=d.stats||{};let ev=(s.events||[]).filter(e=>String(e.device_id).toLowerCase()===String(d.ieee).toLowerCase()).slice(0,8).map(e=>`<div class=\"event\"><span>${ago(e.ts)}</span><span>${esc(e.type)}</span><span>${esc(e.status||'')}</span></div>`).join('')||'<div class=\"msg\">Sin eventos recientes para este dispositivo</div>';box.innerHTML=`<div class=\"deviceTitle\"><div><h2>${esc(d.name)}</h2><span class=\"pill\">${esc(d.ieee)}</span><span class=\"pill\">${d.online?'online':'offline'}</span><span class=\"pill\">${d.is_sleepy?'sleepy':'router'}</span></div><div class=\"actions\"><a class=\"small\" href=\"/devices\">Volver</a><button class=\"small secondary\" onclick=\"devAction('/api/device/reinterview','${d.ieee}')\">Re-entrevistar</button><button class=\"small secondary\" onclick=\"devAction('/api/device/configure','${d.ieee}')\">Reporting</button></div></div><div class=\"grid\">${metric('Estado',esc(d.state),d.online?'ok':'bad')}${metric('Visto hace',ago(d.last_seen_s||0))}${metric('Reporting',reportingLabel(rp)+' '+(rp.received||0)+'/'+(rp.expected||0),rp.configured?'ok':'bad')}${metric('LQI/RSSI',(d.lqi!=null?d.lqi:'-')+' / '+(d.rssi!=null?d.rssi:'-'))}</div><div class=\"panel\">${panel([['Fabricante',esc(d.manufacturer||'-')],['Modelo',esc(d.model||'-')],['Alimentacion',esc(d.power_source||'-')],['Intentos entrevista',String(st.interview_attempts||0)],['Reports ok/igual',(st.report_attr_ok||0)+' / '+(st.report_attr_unchanged||0)],['Read ok/fail',(st.read_rsp_ok||0)+' / '+(st.read_rsp_fail||0)],['Reporting fallos',String(rp.failed||0)]])}</div><h2>Reporting</h2><div class=\"panel\">${reportingFailureRows(d)}</div><h2>Nombre</h2><div class=\"rename\"><input id=\"deviceName\" value=\"${esc(d.name)}\"><button onclick=\"renameDev('${d.ieee}',document.getElementById('deviceName').value)\">Guardar</button></div><h2>Endpoints</h2><table><thead><tr><th>EP</th><th>Perfil</th><th>Tipo</th><th>Clusters in</th><th>Clusters out</th></tr></thead><tbody>${endpointRows(d)||'<tr><td colspan=\"5\">Sin endpoints</td></tr>'}</tbody></table><h2>Atributos cacheados</h2><table><thead><tr><th>Nombre</th><th>Valor</th><th>EP</th><th>Cluster</th><th>Atributo</th><th>Edad</th></tr></thead><tbody>${attrRows(d)||'<tr><td colspan=\"6\">Sin atributos cacheados</td></tr>'}</tbody></table><h2>Eventos</h2><div class=\"events\">${ev}</div>`}"
"function exportStatus(){if(!lastStatus)return;let blob=new Blob([JSON.stringify(lastStatus,null,2)],{type:'application/json'});let url=URL.createObjectURL(blob);let a=document.createElement('a');a.href=url;a.download='esp32-zigbee-status.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)}"
"function pageName(){let p=location.pathname;return p==='/devices'?'devices':p==='/device'?'device':p==='/zigbee'?'zigbee':p==='/network'?'network':p==='/events'?'events':p==='/config'?'config':p==='/actions'?'actions':'status'}"
"function showPage(){let p=pageName();document.querySelectorAll('.page').forEach(x=>x.classList.toggle('active',x.id==='page-'+p));document.querySelectorAll('nav a').forEach(x=>x.classList.toggle('active',x.dataset.page===(p==='device'?'devices':p)))}"
"async function load(forceForm){const s=await j('/api/status');cfg=s.config;"
"lastStatus=s;"
"fillConfigForm(!!forceForm);"
"document.getElementById('metrics').innerHTML=["
"metric('mDNS',cfg.mdns_hostname+'.local'),"
"metric('IP',s.network.has_ip?s.network.ip:'sin IP',s.network.has_ip?'ok':'bad'),"
"metric('Link',s.network.link_up?'activo':'caido',s.network.link_up?'ok':'bad'),"
"metric('Zigbee',s.zigbee.ready?'listo':'iniciando',s.zigbee.ready?'ok':'bad'),"
"metric('Canal/PAN',s.zigbee.ready?(s.zigbee.channel+' / 0x'+Number(s.zigbee.pan_id).toString(16).padStart(4,'0').toUpperCase()):'-'),"
"metric('Hora',s.time.synced?'sincronizada':'pendiente',s.time.synced?'ok':'bad'),"
"metric('Join',s.permit_join.active?('abierto '+s.permit_join.remaining_s+'s'):'cerrado',s.permit_join.active?'ok':''),"
"metric('Dispositivos',s.devices.length+'/'+s.device_capacity),"
"metric('Online',s.online_devices),"
"metric('Sleepy/router',(s.summary.sleepy_devices||0)+'/'+(s.summary.router_devices||0)),"
"metric('Pendientes',(s.summary.reporting_pending||0)+' reporting / '+(s.summary.interview_active||0)+' entrevista'),"
"metric('WebSocket',s.services.websocket_client?'conectado':'sin cliente',s.services.websocket_client?'ok':''),"
"metric('TCP consola',s.services.tcp_console_client?'conectada':'sin cliente',s.services.tcp_console_client?'ok':''),"
"metric('Heap libre',bytes(s.system.free_heap)),"
"metric('Heap min',bytes(s.system.min_free_heap)),"
"metric('Reset',esc(s.system.reset_reason||'-'))"
"].join('');"
"renderConfigTime();renderDevices();renderEvents();renderSystem();renderNetwork();renderZigbee();renderDevicePage();}"
"document.getElementById('save').onclick=async()=>{try{await j('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({mdns_hostname:mdns_hostname.value,mdns_instance:mdns_instance.value,ntp_server:ntp_server.value,timezone:timezone.value,permit_join_duration_s:+permit_join_duration_s.value,report_always_on_max_s:+report_always_on_max_s.value,report_sleepy_max_s:+report_sleepy_max_s.value,presence_grace_s:+presence_grace_s.value})});configDirty=false;setMsg('Configuracion guardada');await load(true);}catch(e){setMsg('Error: '+e.message)}};"
"document.getElementById('refresh').onclick=()=>{configDirty=false;load(true).catch(e=>setMsg('Error: '+e.message))};"
"document.getElementById('autoRefresh').onclick=()=>{autoRefresh=!autoRefresh;document.getElementById('autoRefresh').textContent=autoRefresh?'Pausar auto-refresh':'Reanudar auto-refresh'};"
"document.getElementById('exportJson').onclick=exportStatus;"
"document.getElementById('deviceSearch').oninput=renderDevices;"
"document.getElementById('deviceFilter').onchange=renderDevices;"
"document.getElementById('joinOpen').onclick=async()=>{await j('/api/permit-join',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration_s:+permit_join_duration_s.value||cfg.permit_join_duration_s})});await load();};"
"document.getElementById('joinClose').onclick=async()=>{await j('/api/permit-join',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration_s:0})});await load();};"
"document.getElementById('timeResync').onclick=()=>postAction('/api/time/resync','Sincronizacion iniciada');"
"document.getElementById('configureAll').onclick=()=>postAction('/api/device/configure-all','Reporting global enviado');"
"document.getElementById('closeWs').onclick=()=>postAction('/api/actions/close-ws','WebSocket cerrado');"
"document.getElementById('eraseCache').onclick=()=>confirm('Borrar cache de dispositivos?')&&postAction('/api/actions/erase-device-cache','Cache borrada');"
"document.getElementById('reboot').onclick=()=>confirm('Reiniciar ESP32?')&&postAction('/api/actions/reboot','Reiniciando');"
"document.getElementById('zbReset').onclick=()=>confirm('Resetear red Zigbee y cache? Los dispositivos tendran que volver a unirse.')&&postAction('/api/actions/zigbee-factory-reset','Reset Zigbee iniciado');"
"async function renameDev(id,name){try{await j('/api/device/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id:id,new_name:name})});setMsg('Nombre guardado');await load();}catch(e){setMsg('Error: '+e.message)}}"
"async function devAction(url,id){try{await j(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id:id})});setMsg('Accion enviada');await load();}catch(e){setMsg('Error: '+e.message)}}"
"async function postAction(url,msg){try{await j(url,{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});setMsg(msg);setTimeout(()=>load().catch(()=>{}),800)}catch(e){setMsg('Error: '+e.message)}}"
"populateTimezones();setupConfigDirty();showPage();load(true).catch(e=>setMsg('Error: '+e.message));setInterval(()=>{if(autoRefresh)load(false).catch(()=>{})},5000);"
"</script></body></html>";

static void web_resp_close_after_send(httpd_req_t *req)
{
    if (req) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
}

static esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        web_resp_close_after_send(req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to build JSON");
        return ESP_FAIL;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        ZB_LOG("WEB JSON serialize failed free_heap=%u min_free_heap=%u largest_internal=%u",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        web_resp_close_after_send(req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to serialize JSON");
        return ESP_FAIL;
    }

    web_resp_close_after_send(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t web_send_error_json(httpd_req_t *req,
                                     httpd_err_code_t status,
                                     const char *code,
                                     const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        web_resp_close_after_send(req);
        httpd_resp_send_err(req, status, message ? message : "error");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "error", code ? code : "error");
    cJSON_AddStringToObject(root, "message", message ? message : "error");
    httpd_resp_set_status(req, status == HTTPD_400_BAD_REQUEST ? "400 Bad Request" :
                               status == HTTPD_500_INTERNAL_SERVER_ERROR ? "500 Internal Server Error" :
                               "400 Bad Request");
    return web_send_json(req, root);
}

typedef struct {
    httpd_req_t *req;
    char buf[WEB_JSON_CHUNK_SIZE];
    size_t len;
    esp_err_t err;
} web_json_stream_t;

static void web_json_stream_init(web_json_stream_t *s, httpd_req_t *req)
{
    memset(s, 0, sizeof(*s));
    s->req = req;
    s->err = ESP_OK;
    web_resp_close_after_send(req);
    httpd_resp_set_type(req, "application/json");
}

static esp_err_t web_json_stream_flush(web_json_stream_t *s)
{
    if (!s || s->err != ESP_OK) {
        return s ? s->err : ESP_FAIL;
    }
    if (s->len == 0) {
        return ESP_OK;
    }

    s->err = httpd_resp_send_chunk(s->req, s->buf, s->len);
    s->len = 0;
    return s->err;
}

static void web_json_stream_raw(web_json_stream_t *s, const char *text,
                                size_t len)
{
    if (!s || s->err != ESP_OK || !text) {
        return;
    }

    while (len > 0 && s->err == ESP_OK) {
        size_t avail = sizeof(s->buf) - s->len;
        if (avail == 0) {
            web_json_stream_flush(s);
            continue;
        }
        size_t take = len < avail ? len : avail;
        memcpy(s->buf + s->len, text, take);
        s->len += take;
        text += take;
        len -= take;
    }
}

static void web_json_stream_text(web_json_stream_t *s, const char *text)
{
    web_json_stream_raw(s, text, text ? strlen(text) : 0);
}

static void web_json_stream_char(web_json_stream_t *s, char ch)
{
    web_json_stream_raw(s, &ch, 1);
}

static void web_json_stream_printf(web_json_stream_t *s, const char *fmt, ...)
{
    if (!s || s->err != ESP_OK || !fmt) {
        return;
    }

    char tmp[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || n >= (int)sizeof(tmp)) {
        s->err = ESP_FAIL;
        ZB_LOG("WEB JSON stream format overflow");
        return;
    }
    web_json_stream_raw(s, tmp, (size_t)n);
}

static void web_json_stream_string(web_json_stream_t *s, const char *value)
{
    if (!value) {
        value = "";
    }

    web_json_stream_char(s, '"');
    while (*value && (!s || s->err == ESP_OK)) {
        unsigned char ch = (unsigned char)*value++;
        switch (ch) {
            case '\\':
                web_json_stream_text(s, "\\\\");
                break;
            case '"':
                web_json_stream_text(s, "\\\"");
                break;
            case '\n':
                web_json_stream_text(s, "\\n");
                break;
            case '\r':
                web_json_stream_text(s, "\\r");
                break;
            case '\t':
                web_json_stream_text(s, "\\t");
                break;
            default:
                if (ch < 0x20) {
                    web_json_stream_printf(s, "\\u%04X", ch);
                } else {
                    web_json_stream_char(s, (char)ch);
                }
                break;
        }
    }
    web_json_stream_char(s, '"');
}

static esp_err_t web_json_stream_finish(web_json_stream_t *s)
{
    esp_err_t err = web_json_stream_flush(s);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(s->req, NULL, 0);
    }
    return err;
}

static esp_err_t web_read_json_body(httpd_req_t *req, cJSON **out)
{
    if (!out || req->content_len > WEB_API_BODY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[WEB_API_BODY_MAX + 1];
    size_t received = 0;
    while (received < req->content_len) {
        int got = httpd_req_recv(req, buf + received,
                                 req->content_len - received);
        if (got <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)got;
    }
    buf[received] = '\0';

    *out = cJSON_ParseWithLength(buf, received);
    if (!*out || !cJSON_IsObject(*out)) {
        if (*out) {
            cJSON_Delete(*out);
            *out = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void json_add_config(cJSON *root)
{
    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *config = cJSON_AddObjectToObject(root, "config");
    if (!config) {
        return;
    }
    cJSON_AddStringToObject(config, "mdns_hostname", cfg.mdns_hostname);
    cJSON_AddStringToObject(config, "mdns_instance", cfg.mdns_instance);
    cJSON_AddStringToObject(config, "ntp_server", cfg.ntp_server);
    cJSON_AddStringToObject(config, "timezone", cfg.timezone);
    cJSON_AddNumberToObject(config, "permit_join_duration_s",
                            cfg.permit_join_duration_s);
    cJSON_AddNumberToObject(config, "report_always_on_max_s",
                            cfg.report_always_on_max_s);
    cJSON_AddNumberToObject(config, "report_sleepy_max_s",
                            cfg.report_sleepy_max_s);
    cJSON_AddNumberToObject(config, "presence_grace_s",
                            cfg.presence_grace_s);
}

static esp_err_t web_index_handler(httpd_req_t *req)
{
    web_resp_close_after_send(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_web_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_get_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    json_add_config(root);
    return web_send_json(req, root);
}

static esp_err_t web_put_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = web_read_json_body(req, &body);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "malformed_json",
                                   "Invalid or oversized JSON body");
    }

    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *hostname = cJSON_GetObjectItemCaseSensitive(body, "mdns_hostname");
    cJSON *instance = cJSON_GetObjectItemCaseSensitive(body, "mdns_instance");
    cJSON *ntp_server = cJSON_GetObjectItemCaseSensitive(body, "ntp_server");
    cJSON *timezone = cJSON_GetObjectItemCaseSensitive(body, "timezone");
    cJSON *join_secs = cJSON_GetObjectItemCaseSensitive(body,
                                                        "permit_join_duration_s");
    cJSON *report_always_on = cJSON_GetObjectItemCaseSensitive(body,
                                                               "report_always_on_max_s");
    cJSON *report_sleepy = cJSON_GetObjectItemCaseSensitive(body,
                                                            "report_sleepy_max_s");
    cJSON *presence_grace = cJSON_GetObjectItemCaseSensitive(body,
                                                             "presence_grace_s");

    if (hostname) {
        if (!cJSON_IsString(hostname) || !hostname->valuestring ||
            !app_config_hostname_is_valid(hostname->valuestring)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_hostname",
                                       "mDNS hostname must use letters, numbers and hyphens");
        }
        strncpy(cfg.mdns_hostname, hostname->valuestring,
                sizeof(cfg.mdns_hostname) - 1);
        cfg.mdns_hostname[sizeof(cfg.mdns_hostname) - 1] = '\0';
    }

    if (instance) {
        if (!cJSON_IsString(instance) || !instance->valuestring ||
            instance->valuestring[0] == '\0') {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_instance",
                                       "mDNS instance name cannot be empty");
        }
        strncpy(cfg.mdns_instance, instance->valuestring,
                sizeof(cfg.mdns_instance) - 1);
        cfg.mdns_instance[sizeof(cfg.mdns_instance) - 1] = '\0';
    }

    if (ntp_server) {
        if (!cJSON_IsString(ntp_server) || !ntp_server->valuestring ||
            ntp_server->valuestring[0] == '\0') {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_ntp_server",
                                       "NTP server cannot be empty");
        }
        strncpy(cfg.ntp_server, ntp_server->valuestring,
                sizeof(cfg.ntp_server) - 1);
        cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0';
    }

    if (timezone) {
        if (!cJSON_IsString(timezone) || !timezone->valuestring ||
            timezone->valuestring[0] == '\0') {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_timezone",
                                       "Timezone cannot be empty");
        }
        strncpy(cfg.timezone, timezone->valuestring,
                sizeof(cfg.timezone) - 1);
        cfg.timezone[sizeof(cfg.timezone) - 1] = '\0';
    }

    if (join_secs) {
        if (!cJSON_IsNumber(join_secs)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_join_duration",
                                       "Join duration must be numeric");
        }
        cfg.permit_join_duration_s =
            app_config_clamp_permit_join_duration((uint32_t)join_secs->valuedouble);
    }
    if (report_always_on) {
        if (!cJSON_IsNumber(report_always_on)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_reporting",
                                       "Always-on reporting max must be numeric");
        }
        cfg.report_always_on_max_s = app_config_clamp_report_always_on_max(
            (uint32_t)report_always_on->valuedouble);
    }
    if (report_sleepy) {
        if (!cJSON_IsNumber(report_sleepy)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_reporting",
                                       "Sleepy reporting max must be numeric");
        }
        cfg.report_sleepy_max_s = app_config_clamp_report_sleepy_max(
            (uint32_t)report_sleepy->valuedouble);
    }
    if (presence_grace) {
        if (!cJSON_IsNumber(presence_grace)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_presence",
                                       "Presence grace must be numeric");
        }
        cfg.presence_grace_s = app_config_clamp_presence_grace(
            (uint32_t)presence_grace->valuedouble);
    }
    cJSON_Delete(body);

    err = app_config_save(&cfg);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "save_failed",
                                   esp_err_to_name(err));
    }
    eth_driver_apply_mdns_config();
    time_sync_restart();

    return web_get_config_handler(req);
}

static esp_err_t web_post_permit_join_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = web_read_json_body(req, &body);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "malformed_json",
                                   "Invalid or oversized JSON body");
    }

    app_config_t cfg;
    app_config_get(&cfg);
    uint16_t duration_s = cfg.permit_join_duration_s;
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(body, "duration_s");
    if (duration) {
        if (!cJSON_IsNumber(duration)) {
            cJSON_Delete(body);
            return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                       "invalid_duration",
                                       "duration_s must be numeric");
        }
        if (duration->valuedouble <= 0) {
            duration_s = 0;
        } else {
            duration_s = app_config_clamp_permit_join_duration(
                (uint32_t)duration->valuedouble);
        }
    }
    cJSON_Delete(body);

    button_handler_set_permit_join_duration((uint8_t)duration_s);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", button_handler_permit_join_active());
    cJSON_AddNumberToObject(root, "remaining_s",
                            button_handler_permit_join_remaining_s());
    return web_send_json(req, root);
}

static const char *web_action_error_code(client_action_result_t result)
{
    switch (result) {
        case CLIENT_ACTION_OK:
            return NULL;
        case CLIENT_ACTION_INVALID_ARG:
            return "invalid_argument";
        case CLIENT_ACTION_DEVICE_NOT_FOUND:
            return "device_not_found";
        case CLIENT_ACTION_UNSUPPORTED:
            return "unsupported";
        case CLIENT_ACTION_BUSY:
            return "busy";
        default:
            return "internal_error";
    }
}

static esp_err_t web_send_action_result(httpd_req_t *req,
                                        client_action_result_t result)
{
    if (result != CLIENT_ACTION_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   web_action_error_code(result),
                                   "Device action could not be applied");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    return web_send_json(req, root);
}

static void web_record_device_action_from_id(const char *status,
                                             const char *device_id)
{
    uint64_t ieee = 0;
    utils_str_to_ieee(device_id, &ieee);
    web_events_record_action(status, ieee, device_id);
}

static bool web_get_body_string(cJSON *body, const char *key,
                                const char **out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    if (out) {
        *out = item->valuestring;
    }
    return true;
}

static esp_err_t web_post_device_rename_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = web_read_json_body(req, &body);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "malformed_json",
                                   "Invalid or oversized JSON body");
    }

    const char *device_id = NULL;
    const char *new_name = NULL;
    if (!web_get_body_string(body, "device_id", &device_id) ||
        !web_get_body_string(body, "new_name", &new_name)) {
        cJSON_Delete(body);
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "invalid_argument",
                                   "device_id and new_name are required");
    }

    client_action_result_t result =
        client_actions_rename_device(device_id, new_name);
    if (result == CLIENT_ACTION_OK) {
        web_record_device_action_from_id("rename", device_id);
    }
    cJSON_Delete(body);
    return web_send_action_result(req, result);
}

static esp_err_t web_post_device_reinterview_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = web_read_json_body(req, &body);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "malformed_json",
                                   "Invalid or oversized JSON body");
    }

    const char *device_id = NULL;
    if (!web_get_body_string(body, "device_id", &device_id)) {
        cJSON_Delete(body);
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "invalid_argument",
                                   "device_id is required");
    }

    client_action_result_t result =
        client_actions_interview_device(device_id);
    if (result == CLIENT_ACTION_OK) {
        web_record_device_action_from_id("reinterview", device_id);
    }
    cJSON_Delete(body);
    return web_send_action_result(req, result);
}

static esp_err_t web_post_device_configure_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = web_read_json_body(req, &body);
    if (err != ESP_OK) {
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "malformed_json",
                                   "Invalid or oversized JSON body");
    }

    const char *device_id = NULL;
    if (!web_get_body_string(body, "device_id", &device_id)) {
        cJSON_Delete(body);
        return web_send_error_json(req, HTTPD_400_BAD_REQUEST,
                                   "invalid_argument",
                                   "device_id is required");
    }

    client_action_result_t result =
        client_actions_configure_device(device_id);
    if (result == CLIENT_ACTION_OK) {
        web_record_device_action_from_id("configure_reporting", device_id);
    }
    cJSON_Delete(body);
    return web_send_action_result(req, result);
}

static void delayed_reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void delayed_zigbee_factory_reset_cb(void *arg)
{
    (void)arg;
    esp_zb_factory_reset();
}

static void schedule_delayed_cb(esp_timer_cb_t cb, const char *name)
{
    esp_timer_handle_t timer = NULL;
    esp_timer_create_args_t args = {
        .callback = cb,
        .arg = NULL,
        .name = name ? name : "delayed_action",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 500000);
    }
}

static esp_err_t web_post_reboot_handler(httpd_req_t *req)
{
    (void)req;
    web_events_record_action("reboot", 0, "system");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "rebooting");
    esp_err_t err = web_send_json(req, root);
    schedule_delayed_cb(delayed_reboot_cb, "web_reboot");
    return err;
}

static esp_err_t web_post_erase_device_cache_handler(httpd_req_t *req)
{
    web_events_record_action("erase_device_cache", 0, "system");
    nvs_cache_erase();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", "Device cache erased; reboot recommended");
    return web_send_json(req, root);
}

static esp_err_t web_post_zigbee_factory_reset_handler(httpd_req_t *req)
{
    web_events_record_action("zigbee_factory_reset", 0, "system");
    nvs_cache_erase();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "factory_reset_scheduled");
    esp_err_t err = web_send_json(req, root);
    schedule_delayed_cb(delayed_zigbee_factory_reset_cb, "zb_factory_reset");
    return err;
}

static esp_err_t web_post_close_ws_handler(httpd_req_t *req)
{
    ws_client_session_snapshot_t session;
    ws_client_session_snapshot(&session);
    bool closed = false;

    if (session.active && session.server) {
        httpd_sess_trigger_close(session.server, session.sockfd);
        ws_client_session_close(session.sockfd);
        purge_tx_queue();
        clear_inventory_refresh();
        closed = true;
    }
    if (closed) {
        web_events_record_action("close_ws", 0, "websocket");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "closed", closed);
    return web_send_json(req, root);
}

static esp_err_t web_post_time_resync_handler(httpd_req_t *req)
{
    (void)req;
    web_events_record_action("time_resync", 0, "time");
    time_sync_restart();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "sync_started");
    return web_send_json(req, root);
}

static esp_err_t web_post_configure_all_handler(httpd_req_t *req)
{
    (void)req;
    char ids[MAX_DEVICES][20];
    size_t id_count = 0;

    dm_lock();
    for (int i = 0; i < MAX_DEVICES && id_count < MAX_DEVICES; i++) {
        device_record_t *dev = dm_get_by_index(i);
        if (!dev || dev->state < DEV_STATE_INTERVIEWED) {
            continue;
        }
        utils_ieee_to_str(dev->ieee_addr, ids[id_count], sizeof(ids[id_count]));
        id_count++;
    }
    dm_unlock();

    size_t queued = 0;
    size_t busy = 0;
    for (size_t i = 0; i < id_count; i++) {
        client_action_result_t result = client_actions_configure_device(ids[i]);
        if (result == CLIENT_ACTION_OK) {
            queued++;
        } else if (result == CLIENT_ACTION_BUSY) {
            busy++;
        }
    }
    web_events_record_action("configure_all", 0, "reporting");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "eligible", id_count);
    cJSON_AddNumberToObject(root, "queued", queued);
    cJSON_AddNumberToObject(root, "busy", busy);
    return web_send_json(req, root);
}

static void format_zb_addr(const uint8_t addr[8], char *buf, size_t len)
{
    if (!addr || !buf || len == 0) {
        return;
    }

    snprintf(buf, len, "%02X%02X%02X%02X%02X%02X%02X%02X",
             addr[7], addr[6], addr[5], addr[4],
             addr[3], addr[2], addr[1], addr[0]);
}

static uint32_t web_read_u32_le(const uint8_t value[8], int len)
{
    uint32_t out = 0;
    if (len > 4) {
        len = 4;
    }
    for (int i = 0; i < len; i++) {
        out |= ((uint32_t)value[i] << (8 * i));
    }
    return out;
}

static int web_zcl_type_size(uint8_t type)
{
    switch (type) {
        case 0x10:
        case 0x18:
        case 0x20:
        case 0x28:
            return 1;
        case 0x19:
        case 0x21:
        case 0x29:
            return 2;
        case 0x2A:
        case 0x22:
            return 3;
        case 0x23:
        case 0x2B:
            return 4;
        default:
            return 1;
    }
}

static int32_t web_signed_value(uint32_t raw, int len)
{
    if (len == 1) {
        return (int8_t)(raw & 0xFFu);
    }
    if (len == 2) {
        return (int16_t)(raw & 0xFFFFu);
    }
    if (len == 3 && (raw & 0x800000u)) {
        return (int32_t)(raw | 0xFF000000u);
    }
    return (int32_t)raw;
}

static void web_json_stream_attr_value(web_json_stream_t *s,
                                       const attr_cache_entry_t *attr)
{
    int len = web_zcl_type_size(attr->attr_type);
    uint32_t raw = web_read_u32_le(attr->value, len);
    int32_t sval = web_signed_value(raw, len);

    switch (attr->cluster_id) {
        case 0x0006:
            web_json_stream_string(s, raw ? "ON" : "OFF");
            break;
        case 0x0008:
            web_json_stream_printf(s, "%lu", (unsigned long)(raw & 0xFFu));
            break;
        case 0x0402:
            web_json_stream_printf(s, "%.2f", (double)sval / 100.0);
            break;
        case 0x0405:
            web_json_stream_printf(s, "%.2f", (double)(raw & 0xFFFFu) / 100.0);
            break;
        case 0x0403:
            web_json_stream_printf(s, "%.1f", (double)sval / 10.0);
            break;
        case 0x0400:
            web_json_stream_printf(s, "%lu", (unsigned long)(raw & 0xFFFFu));
            break;
        case 0x0406:
            web_json_stream_text(s, (raw & 0x01u) ? "true" : "false");
            break;
        case 0x0001:
            if (attr->attr_id == 0x0020) {
                web_json_stream_printf(s, "%lu", (unsigned long)((raw & 0xFFu) * 100u));
            } else {
                web_json_stream_printf(s, "%lu", (unsigned long)((raw & 0xFFu) / 2u));
            }
            break;
        case 0x0B04:
            web_json_stream_printf(s, "%.1f", (double)sval / 10.0);
            break;
        case 0x0500:
            web_json_stream_text(s, (raw & 0x01u) == 0 ? "true" : "false");
            break;
        default:
            web_json_stream_printf(s, "%lu", (unsigned long)raw);
            break;
    }
}

static void web_json_stream_device_readings(web_json_stream_t *s,
                                            const attr_cache_entry_t *attrs,
                                            size_t count)
{
    bool first = true;

    web_json_stream_text(s, "\"readings\":{");
    if (attrs) {
        for (size_t i = 0; i < count; i++) {
            ws_attr_meta_t meta;
            if (!ws_model_attr_meta(attrs[i].cluster_id, attrs[i].attr_id, &meta)) {
                continue;
            }

            if (!first) {
                web_json_stream_char(s, ',');
            }
            first = false;
            web_json_stream_string(s, meta.name);
            web_json_stream_text(s, ":{\"value\":");
            web_json_stream_attr_value(s, &attrs[i]);
            if (meta.unit) {
                web_json_stream_text(s, ",\"unit\":");
                web_json_stream_string(s, meta.unit);
            }
            web_json_stream_printf(s,
                                   ",\"endpoint\":%u,\"cluster_id\":%u,"
                                   "\"attr_id\":%u,\"ts\":%lu}",
                                   attrs[i].endpoint_id, attrs[i].cluster_id,
                                   attrs[i].attr_id,
                                   (unsigned long)(attrs[i].last_update_ms / 1000u));
        }
    }
    web_json_stream_char(s, '}');
}

static void web_json_stream_device_attrs(web_json_stream_t *s,
                                         const attr_cache_entry_t *attrs,
                                         size_t count)
{
    web_json_stream_text(s, ",\"attrs\":[");
    if (attrs) {
        for (size_t i = 0; i < count; i++) {
            char raw[17];
            for (int b = 0; b < 8; b++) {
                snprintf(&raw[b * 2], sizeof(raw) - (size_t)(b * 2),
                         "%02X", attrs[i].value[b]);
            }
            raw[16] = '\0';

            if (i > 0) {
                web_json_stream_char(s, ',');
            }
            web_json_stream_printf(s,
                                   "{\"endpoint\":%u,\"cluster_id\":%u,"
                                   "\"attr_id\":%u,\"attr_type\":%u,\"raw\":",
                                   attrs[i].endpoint_id, attrs[i].cluster_id,
                                   attrs[i].attr_id, attrs[i].attr_type);
            web_json_stream_string(s, raw);
            web_json_stream_printf(s, ",\"ts\":%lu}",
                                   (unsigned long)(attrs[i].last_update_ms / 1000u));
        }
    }
    web_json_stream_char(s, ']');
}

static void web_json_stream_cluster_array(web_json_stream_t *s,
                                          const char *name,
                                          const uint16_t *clusters,
                                          uint8_t count)
{
    web_json_stream_string(s, name);
    web_json_stream_text(s, ":[");
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) {
            web_json_stream_char(s, ',');
        }
        web_json_stream_printf(s, "%u", clusters[i]);
    }
    web_json_stream_char(s, ']');
}

static void web_json_stream_device_endpoints(web_json_stream_t *s,
                                             const device_record_t *dev)
{
    web_json_stream_text(s, "\"endpoints\":[");
    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        const endpoint_record_t *ep = &dev->endpoints[i];
        if (i > 0) {
            web_json_stream_char(s, ',');
        }
        web_json_stream_printf(s,
                               "{\"id\":%u,\"profile_id\":%u,\"device_id\":%u,"
                               "\"device_type\":",
                               ep->endpoint_id, ep->profile_id, ep->device_id);
        web_json_stream_string(s, utils_device_type_name(ep->device_id));
        web_json_stream_char(s, ',');
        web_json_stream_cluster_array(s, "in_clusters",
                                      ep->in_clusters, ep->in_cluster_count);
        web_json_stream_char(s, ',');
        web_json_stream_cluster_array(s, "out_clusters",
                                      ep->out_clusters, ep->out_cluster_count);
        web_json_stream_char(s, '}');
    }
    web_json_stream_char(s, ']');
}

static const char *web_report_cfg_reason(uint8_t result)
{
    switch ((report_cfg_result_t)result) {
        case REPORT_CFG_RESULT_FAIL:
            return "fail";
        case REPORT_CFG_RESULT_MISSING:
            return "missing";
        case REPORT_CFG_RESULT_BIND_FAIL:
            return "bind_fail";
        case REPORT_CFG_RESULT_WRITE_FAIL:
            return "write_fail";
        default:
            return "unknown";
    }
}

static void web_json_stream_reporting_failures(web_json_stream_t *s,
                                               const device_record_t *dev)
{
    bool first = true;

    web_json_stream_text(s, "\"failures\":[");
    if (dev) {
        for (uint8_t i = 0; i < dev->report_cfg_record_count; i++) {
            const report_cfg_record_t *record = &dev->report_cfg_records[i];
            if (record->result != REPORT_CFG_RESULT_FAIL &&
                record->result != REPORT_CFG_RESULT_MISSING &&
                record->result != REPORT_CFG_RESULT_BIND_FAIL &&
                record->result != REPORT_CFG_RESULT_WRITE_FAIL) {
                continue;
            }
            if (!first) {
                web_json_stream_char(s, ',');
            }
            first = false;
            web_json_stream_printf(s,
                                   "{\"endpoint\":%u,\"cluster_id\":%u,"
                                   "\"attr_id\":%u,\"status\":%u,\"reason\":",
                                   record->endpoint, record->cluster_id,
                                   record->attr_id, record->status);
            web_json_stream_string(s, web_report_cfg_reason(record->result));
            web_json_stream_text(s, ",\"cluster_name\":");
            web_json_stream_string(s, utils_cluster_name(record->cluster_id));
            web_json_stream_char(s, '}');
        }
    }
    web_json_stream_char(s, ']');
    web_json_stream_printf(s, ",\"overflow\":%s",
                           dev && dev->report_cfg_record_overflow
                               ? "true"
                               : "false");
}

static void web_json_stream_recent_events(web_json_stream_t *s)
{
    web_event_entry_t snapshot[WEB_EVENT_HISTORY_LEN];
    uint32_t seq = 0;
    bool first = true;

    portENTER_CRITICAL(&s_web_event_lock);
    memcpy(snapshot, s_web_events, sizeof(snapshot));
    seq = s_web_event_seq;
    portEXIT_CRITICAL(&s_web_event_lock);

    web_json_stream_text(s, "\"events\":[");
    uint32_t count = seq < WEB_EVENT_HISTORY_LEN ? seq : WEB_EVENT_HISTORY_LEN;
    for (uint32_t n = 0; n < count; n++) {
        uint32_t idx = (seq - 1u - n) % WEB_EVENT_HISTORY_LEN;
        const web_event_entry_t *entry = &snapshot[idx];
        if (!entry->in_use) {
            continue;
        }

        char ieee[20] = "";
        if (entry->ieee != 0) {
            utils_ieee_to_str(entry->ieee, ieee, sizeof(ieee));
        }
        if (!first) {
            web_json_stream_char(s, ',');
        }
        first = false;
        web_json_stream_printf(s, "{\"ts\":%lu,\"type\":",
                               (unsigned long)entry->ts_s);
        web_json_stream_string(s, web_event_type_name(entry->type));
        web_json_stream_text(s, ",\"device_id\":");
        web_json_stream_string(s, ieee);
        web_json_stream_text(s, ",\"name\":");
        web_json_stream_string(s, entry->friendly_name);
        if (entry->status[0]) {
            web_json_stream_text(s, ",\"status\":");
            web_json_stream_string(s, entry->status);
        }
        if (entry->type == ZB_EVT_AVAILABILITY ||
            entry->type == ZB_EVT_DEVICE_JOINED ||
            entry->type == ZB_EVT_DEVICE_LEAVE) {
            web_json_stream_printf(s, ",\"online\":%s",
                                   entry->online ? "true" : "false");
        }
        if (entry->type == ZB_EVT_ATTR_CHANGED) {
            web_json_stream_printf(s,
                                   ",\"endpoint\":%u,\"cluster_id\":%u,"
                                   "\"attr_id\":%u",
                                   entry->endpoint, entry->cluster_id,
                                   entry->attr_id);
        }
        if (entry->type == ZB_EVT_PERMIT_JOIN) {
            web_json_stream_printf(s, ",\"duration\":%u",
                                   entry->permit_join_duration);
        }
        web_json_stream_char(s, '}');
    }
    web_json_stream_char(s, ']');
}

static int32_t web_local_utc_offset_seconds(time_t now)
{
    struct tm local_tm = {0};
    struct tm utc_tm = {0};

    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);

    time_t local_epoch = mktime(&local_tm);
    time_t utc_epoch = mktime(&utc_tm);
    return (int32_t)difftime(local_epoch, utc_epoch);
}

static void web_format_utc_offset(int32_t offset_s, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    char sign = offset_s >= 0 ? '+' : '-';
    uint32_t abs_s = offset_s >= 0 ? (uint32_t)offset_s
                                   : (uint32_t)(-offset_s);
    uint32_t hours = abs_s / 3600u;
    uint32_t minutes = (abs_s % 3600u) / 60u;
    snprintf(buf, len, "UTC%c%02lu:%02lu", sign,
             (unsigned long)hours, (unsigned long)minutes);
}

static esp_err_t web_get_status_handler(httpd_req_t *req)
{
    web_json_stream_t out;
    app_config_t cfg;
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *partition = esp_ota_get_running_partition();
    esp_reset_reason_t reset_reason = esp_reset_reason();

    app_config_get(&cfg);
    web_json_stream_init(&out, req);

    web_json_stream_text(&out, "{\"config\":{\"mdns_hostname\":");
    web_json_stream_string(&out, cfg.mdns_hostname);
    web_json_stream_text(&out, ",\"mdns_instance\":");
    web_json_stream_string(&out, cfg.mdns_instance);
    web_json_stream_text(&out, ",\"ntp_server\":");
    web_json_stream_string(&out, cfg.ntp_server);
    web_json_stream_text(&out, ",\"timezone\":");
    web_json_stream_string(&out, cfg.timezone);
    web_json_stream_printf(&out,
                           ",\"permit_join_duration_s\":%u,"
                           "\"report_always_on_max_s\":%lu,"
                           "\"report_sleepy_max_s\":%lu,"
                           "\"presence_grace_s\":%lu}",
                           cfg.permit_join_duration_s,
                           (unsigned long)cfg.report_always_on_max_s,
                           (unsigned long)cfg.report_sleepy_max_s,
                           (unsigned long)cfg.presence_grace_s);

    web_json_stream_printf(&out,
                           ",\"system\":{\"uptime_s\":%lu,\"free_heap\":%u,"
                           "\"min_free_heap\":%u,\"free_internal_heap\":%u,"
                           "\"reset_reason\":",
                           (unsigned long)(utils_uptime_ms() / 1000u),
                           (unsigned)esp_get_free_heap_size(),
                           (unsigned)esp_get_minimum_free_heap_size(),
                           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    web_json_stream_string(&out, reset_reason_name(reset_reason));
    if (app) {
        web_json_stream_text(&out, ",\"app_name\":");
        web_json_stream_string(&out, app->project_name);
        web_json_stream_text(&out, ",\"app_version\":");
        web_json_stream_string(&out, app->version);
        web_json_stream_text(&out, ",\"idf_version\":");
        web_json_stream_string(&out, app->idf_ver);
        web_json_stream_text(&out, ",\"build_date\":");
        web_json_stream_string(&out, app->date);
        web_json_stream_text(&out, ",\"build_time\":");
        web_json_stream_string(&out, app->time);
    }
    if (partition) {
        web_json_stream_text(&out, ",\"partition_label\":");
        web_json_stream_string(&out, partition->label);
        web_json_stream_printf(&out,
                               ",\"partition_address\":%lu,"
                               "\"partition_size\":%lu",
                               (unsigned long)partition->address,
                               (unsigned long)partition->size);
    }
    web_json_stream_char(&out, '}');

    eth_driver_status_t eth_status;
    eth_driver_get_status(&eth_status);
    web_json_stream_printf(&out,
                           ",\"network\":{\"started\":%s,\"link_up\":%s,"
                           "\"has_ip\":%s,\"mac\":",
                           eth_status.started ? "true" : "false",
                           eth_status.link_up ? "true" : "false",
                           eth_status.has_ip ? "true" : "false");
    web_json_stream_string(&out, eth_status.mac);
    web_json_stream_text(&out, ",\"ip\":");
    web_json_stream_string(&out, eth_status.ip);
    web_json_stream_text(&out, ",\"netmask\":");
    web_json_stream_string(&out, eth_status.netmask);
    web_json_stream_text(&out, ",\"gateway\":");
    web_json_stream_string(&out, eth_status.gateway);
    web_json_stream_char(&out, '}');

    web_json_stream_printf(&out,
                           ",\"services\":{\"websocket_client\":%s,"
                           "\"tcp_console_client\":%s}",
                           ws_client_session_is_active() ? "true" : "false",
                           tcp_console_is_connected() ? "true" : "false");

    time_sync_status_t time_status;
    time_sync_get_status(&time_status);
    bool valid_time = utils_wall_time_valid();
    web_json_stream_printf(&out,
                           ",\"time\":{\"started\":%s,\"synced\":%s,\"server\":",
                           time_status.started ? "true" : "false",
                           time_status.synced ? "true" : "false");
    web_json_stream_string(&out, time_status.server);
    web_json_stream_text(&out, ",\"timezone\":");
    web_json_stream_string(&out, time_status.timezone);
    web_json_stream_printf(&out,
                           ",\"last_sync_uptime_s\":%lu,\"valid\":%s",
                           (unsigned long)time_status.last_sync_uptime_s,
                           valid_time ? "true" : "false");
    if (valid_time) {
        time_t now = time(NULL);
        struct tm local_tm = {0};
        struct tm utc_tm = {0};
        char local_buf[32];
        char utc_buf[32];
        char offset_buf[12];

        localtime_r(&now, &local_tm);
        gmtime_r(&now, &utc_tm);
        strftime(local_buf, sizeof(local_buf), "%Y-%m-%d %H:%M:%S",
                 &local_tm);
        strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M:%S",
                 &utc_tm);
        web_format_utc_offset(web_local_utc_offset_seconds(now),
                              offset_buf, sizeof(offset_buf));
        web_json_stream_printf(&out, ",\"current_epoch\":%lu",
                               (unsigned long)now);
        web_json_stream_text(&out, ",\"current_local\":");
        web_json_stream_string(&out, local_buf);
        web_json_stream_text(&out, ",\"current_utc\":");
        web_json_stream_string(&out, utc_buf);
        web_json_stream_text(&out, ",\"utc_offset\":");
        web_json_stream_string(&out, offset_buf);
    }
    web_json_stream_char(&out, '}');

    web_json_stream_printf(&out,
                           ",\"permit_join\":{\"active\":%s,\"remaining_s\":%lu}",
                           button_handler_permit_join_active() ? "true" : "false",
                           (unsigned long)button_handler_permit_join_remaining_s());

    bool zb_ready = zigbee_core_is_ready();
    web_json_stream_printf(&out, ",\"zigbee\":{\"ready\":%s",
                           zb_ready ? "true" : "false");
    if (zb_ready && esp_zb_lock_acquire(pdMS_TO_TICKS(WEB_ZB_LOCK_WAIT_MS))) {
        esp_zb_ieee_addr_t ext_pan = {0};
        esp_zb_ieee_addr_t coord_ieee = {0};
        char ext_pan_str[17];
        char coord_ieee_str[17];
        web_json_stream_printf(&out, ",\"channel\":%u,\"pan_id\":%u",
                               esp_zb_get_current_channel(), esp_zb_get_pan_id());
        esp_zb_get_extended_pan_id(ext_pan);
        esp_zb_get_long_address(coord_ieee);
        format_zb_addr(ext_pan, ext_pan_str, sizeof(ext_pan_str));
        format_zb_addr(coord_ieee, coord_ieee_str, sizeof(coord_ieee_str));
        web_json_stream_text(&out, ",\"ext_pan_id\":");
        web_json_stream_string(&out, ext_pan_str);
        web_json_stream_text(&out, ",\"coordinator_ieee\":");
        web_json_stream_string(&out, coord_ieee_str);
        esp_zb_lock_release();
    }
    web_json_stream_char(&out, '}');

    web_json_stream_printf(&out, ",\"device_capacity\":%u,\"device_count\":%u",
                           (unsigned)MAX_DEVICES, (unsigned)dm_count());
    web_json_stream_text(&out, ",\"devices\":[");
    uint8_t online_count = 0;
    uint8_t sleepy_count = 0;
    uint8_t router_count = 0;
    uint8_t reporting_pending_count = 0;
    uint8_t interview_active_count = 0;
    bool first_device = true;
    device_record_t *dev_snapshot = malloc(sizeof(*dev_snapshot));
    attr_cache_entry_t *attr_snapshot =
        malloc(sizeof(*attr_snapshot) * MAX_ATTR_CACHE);
    if (!dev_snapshot) {
        ZB_LOG("WEB status: no heap for device snapshot free_heap=%u largest_internal=%u",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!attr_snapshot) {
        ZB_LOG("WEB status: no heap for attr snapshot free_heap=%u largest_internal=%u",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    for (int i = 0; i < MAX_DEVICES; i++) {
        bool has_device = false;
        if (dev_snapshot) {
            dm_lock();
            device_record_t *dev = dm_get_by_index(i);
            if (dev) {
                *dev_snapshot = *dev;
                has_device = true;
            }
            dm_unlock();
        }
        if (!has_device) {
            continue;
        }
        if (dev_snapshot->online) {
            online_count++;
        }
        if (dev_snapshot->is_sleepy) {
            sleepy_count++;
        } else {
            router_count++;
        }
        if (!dev_snapshot->reporting_configured || dev_snapshot->report_cfg_in_progress) {
            reporting_pending_count++;
        }
        if (dev_snapshot->state == DEV_STATE_INTERVIEWING) {
            interview_active_count++;
        }

        char ieee[20];
        utils_ieee_to_str(dev_snapshot->ieee_addr, ieee, sizeof(ieee));
        if (!first_device) {
            web_json_stream_char(&out, ',');
        }
        first_device = false;

        web_json_stream_text(&out, "{\"ieee\":");
        web_json_stream_string(&out, ieee);
        web_json_stream_text(&out, ",\"name\":");
        web_json_stream_string(&out, dm_display_name(dev_snapshot));
        web_json_stream_printf(&out,
                               ",\"online\":%s,\"is_sleepy\":%s,\"state\":",
                               dev_snapshot->online ? "true" : "false",
                               dev_snapshot->is_sleepy ? "true" : "false");
        web_json_stream_string(&out,
                               utils_device_state_name((int)dev_snapshot->state));
        web_json_stream_text(&out, ",\"manufacturer\":");
        web_json_stream_string(&out, dev_snapshot->manufacturer);
        web_json_stream_text(&out, ",\"model\":");
        web_json_stream_string(&out, dev_snapshot->model);
        web_json_stream_text(&out, ",\"power_source\":");
        web_json_stream_string(&out,
                               utils_power_source_name(dev_snapshot->power_source));
        web_json_stream_printf(&out,
                               ",\"last_seen_s\":%lu,\"reporting\":{"
                               "\"configured\":%s,\"in_progress\":%s,"
                               "\"expected\":%u,\"received\":%u,\"failed\":%u,",
                               (unsigned long)(dev_snapshot->last_seen_ms / 1000u),
                               dev_snapshot->reporting_configured ? "true" : "false",
                               dev_snapshot->report_cfg_in_progress ? "true" : "false",
                               dev_snapshot->report_cfg_expected,
                               dev_snapshot->report_cfg_received,
                               dev_snapshot->report_cfg_failed);
        web_json_stream_reporting_failures(&out, dev_snapshot);
        web_json_stream_printf(&out,
                               "},\"stats\":{\"report_attr_ok\":%lu,"
                               "\"report_attr_unchanged\":%lu,"
                               "\"read_rsp_ok\":%lu,\"read_rsp_fail\":%lu,"
                               "\"interview_attempts\":%lu}",
                               (unsigned long)dev_snapshot->report_attr_ok,
                               (unsigned long)dev_snapshot->report_attr_unchanged,
                               (unsigned long)dev_snapshot->read_rsp_ok,
                               (unsigned long)dev_snapshot->read_rsp_fail,
                               (unsigned long)dev_snapshot->interview_attempts);
        if (dev_snapshot->radio_metrics_valid) {
            web_json_stream_printf(&out, ",\"lqi\":%u,\"rssi\":%d",
                                   dev_snapshot->last_lqi,
                                   (int)dev_snapshot->last_rssi);
        }
        web_json_stream_char(&out, ',');
        web_json_stream_device_endpoints(&out, dev_snapshot);
        web_json_stream_char(&out, ',');
        size_t attr_count = 0;
        if (attr_snapshot) {
            size_t total = zcl_get_cached_attrs(dev_snapshot->ieee_addr, attr_snapshot,
                                                MAX_ATTR_CACHE);
            attr_count = total > MAX_ATTR_CACHE ? MAX_ATTR_CACHE : total;
        }
        web_json_stream_device_readings(&out, attr_snapshot, attr_count);
        web_json_stream_device_attrs(&out, attr_snapshot, attr_count);
        web_json_stream_char(&out, '}');
    }
    free(dev_snapshot);
    free(attr_snapshot);

    web_json_stream_printf(&out,
                           "],\"online_devices\":%u,\"summary\":{"
                           "\"sleepy_devices\":%u,\"router_devices\":%u,"
                           "\"reporting_pending\":%u,\"interview_active\":%u},",
                           online_count, sleepy_count, router_count,
                           reporting_pending_count, interview_active_count);
    web_json_stream_recent_events(&out);
    web_json_stream_text(&out, "}");

    esp_err_t err = web_json_stream_finish(&out);
    if (err != ESP_OK) {
        ZB_LOG("WEB status stream failed err=0x%X free_heap=%u largest_internal=%u",
               err, (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return err;
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WS_HTTP_PORT;
    config.ctrl_port = WS_HTTP_PORT + 1;
    config.lru_purge_enable = true;
    config.close_fn = ws_close_fn;
    config.send_wait_timeout = WS_SEND_WAIT_TIMEOUT_S;
    config.recv_wait_timeout = WS_RECV_WAIT_TIMEOUT_S;
    config.stack_size = WS_HTTPD_STACK_SIZE;
    config.max_uri_handlers = 28;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t ws_uri = {
        .uri = WS_URI,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &index_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t devices_page_uri = {
        .uri = "/devices",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &devices_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t events_page_uri = {
        .uri = "/events",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &events_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t config_page_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &config_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t actions_page_uri = {
        .uri = "/actions",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &actions_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t device_page_uri = {
        .uri = "/device",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &device_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t zigbee_page_uri = {
        .uri = "/zigbee",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &zigbee_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t network_page_uri = {
        .uri = "/network",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &network_page_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = web_get_status_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &status_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t get_config_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = web_get_config_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &get_config_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t put_config_uri = {
        .uri = "/api/config",
        .method = HTTP_PUT,
        .handler = web_put_config_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &put_config_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t permit_join_uri = {
        .uri = "/api/permit-join",
        .method = HTTP_POST,
        .handler = web_post_permit_join_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &permit_join_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t device_rename_uri = {
        .uri = "/api/device/rename",
        .method = HTTP_POST,
        .handler = web_post_device_rename_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &device_rename_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t device_reinterview_uri = {
        .uri = "/api/device/reinterview",
        .method = HTTP_POST,
        .handler = web_post_device_reinterview_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &device_reinterview_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t device_configure_uri = {
        .uri = "/api/device/configure",
        .method = HTTP_POST,
        .handler = web_post_device_configure_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &device_configure_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t configure_all_uri = {
        .uri = "/api/device/configure-all",
        .method = HTTP_POST,
        .handler = web_post_configure_all_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &configure_all_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t time_resync_uri = {
        .uri = "/api/time/resync",
        .method = HTTP_POST,
        .handler = web_post_time_resync_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &time_resync_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t reboot_uri = {
        .uri = "/api/actions/reboot",
        .method = HTTP_POST,
        .handler = web_post_reboot_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &reboot_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t erase_cache_uri = {
        .uri = "/api/actions/erase-device-cache",
        .method = HTTP_POST,
        .handler = web_post_erase_device_cache_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &erase_cache_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t close_ws_uri = {
        .uri = "/api/actions/close-ws",
        .method = HTTP_POST,
        .handler = web_post_close_ws_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &close_ws_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t zb_factory_reset_uri = {
        .uri = "/api/actions/zigbee-factory-reset",
        .method = HTTP_POST,
        .handler = web_post_zigbee_factory_reset_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(s_server, &zb_factory_reset_uri);
}

static void ws_task(void *arg)
{
    (void)arg;

    ZB_LOG("WS task: waiting for Ethernet IP");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    esp_err_t err = start_server();
    if (err != ESP_OK) {
        ZB_LOG("WS server start failed err=0x%X", err);
        vTaskDelete(NULL);
        return;
    }

    ZB_LOG("WS server listening on ws://<gateway>:%u%s",
           WS_HTTP_PORT, WS_URI);
    ZB_LOG("WS TX policy: max_queue=%u critical evicts telemetry/structural, structural evicts telemetry, telemetry drops when full",
           (unsigned)WS_TX_QUEUE_LEN);

    for (;;) {
        uint32_t notify = 0;
        if (xTaskNotifyWait(0,
                            WS_NOTIFY_AUTONOMOUS_SYNC |
                            WS_NOTIFY_INVENTORY_REFRESH,
                            &notify,
                            pdMS_TO_TICKS(100)) == pdTRUE) {
            if (notify & WS_NOTIFY_AUTONOMOUS_SYNC) {
                start_autonomous_debug_stream();
            }
            if (notify & WS_NOTIFY_INVENTORY_REFRESH) {
                ZB_LOG("WS inventory refresh: pending");
            }
        }
        if (take_inventory_refresh_if_due()) {
            send_inventory_refresh_now();
        }
        drain_tx_queue();
    }
}

void ws_transport_init(EventGroupHandle_t eth_ready_eg)
{
    s_eth_eg = eth_ready_eg;
    s_tx_lock = xSemaphoreCreateMutex();
    configASSERT(s_tx_lock);
    ws_client_session_init();
    zb_events_register(web_events_record_zigbee);

    xTaskCreate(ws_task, "ws_task", WS_TASK_STACK, NULL,
                WS_TASK_PRIORITY, &s_task_handle);
}

bool ws_transport_notify_inventory(void)
{
    return schedule_inventory_refresh();
}

bool ws_transport_notify_state_change(const zb_event_t *evt)
{
    if (!ws_client_session_is_active()) {
        return false;
    }
    return ws_protocol_send_zigbee_event(enqueue_payload_telemetry, NULL, evt);
}

bool ws_transport_notify_event(const zb_event_t *evt)
{
    if (!ws_client_session_is_active()) {
        return false;
    }
    return ws_protocol_send_zigbee_event(enqueue_payload_structural, NULL, evt);
}

bool ws_transport_send_cmd_result(uint32_t reply_to, const char *status,
                                  bool applied, const char *error_code)
{
    if (!ws_client_session_is_active()) {
        return false;
    }
    return ws_protocol_send_cmd_result(enqueue_payload_critical, NULL, reply_to,
                                       status, applied, error_code);
}
