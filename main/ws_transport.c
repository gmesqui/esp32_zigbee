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
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void clear_inventory_refresh(void)
{
    portENTER_CRITICAL(&s_refresh_lock);
    s_inventory_refresh_pending = false;
    s_inventory_refresh_due_tick = 0;
    portEXIT_CRITICAL(&s_refresh_lock);
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
}

static const char s_web_index_html[] =
"<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESP32 Zigbee</title><style>"
":root{color-scheme:light dark;font-family:system-ui,-apple-system,Segoe UI,sans-serif}"
"body{margin:0;background:#f5f7f9;color:#172026}"
"header{background:#172026;color:#fff;padding:18px 22px}"
"main{max-width:1120px;margin:0 auto;padding:18px;display:grid;gap:16px}"
"section{background:#fff;border:1px solid #d8dee4;border-radius:8px;padding:16px}"
"h1{font-size:22px;margin:0}h2{font-size:17px;margin:0 0 12px}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}"
".metric{border:1px solid #e4e8ec;border-radius:6px;padding:10px;background:#fbfcfd}"
".label{font-size:12px;color:#66727c}.value{font-size:20px;font-weight:650;margin-top:4px}"
"label{display:grid;gap:5px;font-size:13px;color:#394650}input{font:inherit;padding:9px;border:1px solid #c8d0d8;border-radius:6px}"
"button{font:inherit;border:0;border-radius:6px;padding:9px 12px;background:#145c9e;color:#fff;cursor:pointer}"
"button.secondary{background:#596775}button.warn{background:#a33b21}.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}"
"table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:left;padding:8px;border-bottom:1px solid #e6ebef}"
".rename{display:flex;gap:6px;align-items:center}.rename input{min-width:120px;max-width:190px;padding:7px}.small{padding:7px 9px;font-size:12px}"
".ok{color:#167145}.bad{color:#a33b21}.msg{min-height:20px;color:#596775;font-size:13px}"
"@media (prefers-color-scheme:dark){body{background:#11171c;color:#edf2f6}section{background:#172026;border-color:#2a3540}.metric{background:#1d2730;border-color:#2f3a45}.label{color:#aeb9c2}input{background:#11171c;color:#edf2f6;border-color:#3b4854}th,td{border-color:#2a3540}}"
"</style></head><body><header><h1>ESP32 Zigbee Coordinator</h1></header><main>"
"<section><h2>Estado</h2><div class=\"grid\" id=\"metrics\"></div></section>"
"<section><h2>Configuracion</h2><div class=\"grid\">"
"<label>Nombre mDNS<input id=\"mdns_hostname\" maxlength=\"31\"></label>"
"<label>Nombre visible<input id=\"mdns_instance\" maxlength=\"63\"></label>"
"<label>Servidor NTP<input id=\"ntp_server\" maxlength=\"63\"></label>"
"<label>Zona horaria POSIX<input id=\"timezone\" maxlength=\"47\"></label>"
"<label>Join por defecto (s)<input id=\"permit_join_duration_s\" type=\"number\" min=\"10\" max=\"254\"></label>"
"</div><h2>Reporting</h2><div class=\"grid\">"
"<label>Max always-on (s)<input id=\"report_always_on_max_s\" type=\"number\" min=\"30\" max=\"3600\"></label>"
"<label>Max sleepy (s)<input id=\"report_sleepy_max_s\" type=\"number\" min=\"300\" max=\"43200\"></label>"
"<label>Margen presencia (s)<input id=\"presence_grace_s\" type=\"number\" min=\"5\" max=\"3600\"></label>"
"</div><div class=\"actions\"><button id=\"save\">Guardar</button><button class=\"secondary\" id=\"refresh\">Actualizar</button></div><div class=\"msg\" id=\"msg\"></div></section>"
"<section><h2>Join</h2><div class=\"actions\"><button id=\"joinOpen\">Abrir join</button><button class=\"warn\" id=\"joinClose\">Cerrar join</button></div></section>"
"<section><h2>Acciones</h2><div class=\"actions\"><button class=\"secondary\" id=\"timeResync\">Sincronizar hora</button><button class=\"secondary\" id=\"configureAll\">Reconfigurar reporting en todos</button><button class=\"secondary\" id=\"closeWs\">Cerrar WebSocket</button><button class=\"warn\" id=\"eraseCache\">Borrar cache dispositivos</button><button class=\"warn\" id=\"reboot\">Reiniciar</button><button class=\"warn\" id=\"zbReset\">Reset red Zigbee</button></div></section>"
"<section><h2>Dispositivos</h2><table><thead><tr><th>Nombre</th><th>IEEE</th><th>Estado</th><th>Lecturas</th><th>Modelo</th><th>Acciones</th></tr></thead><tbody id=\"devices\"></tbody></table></section>"
"</main><script>"
"let cfg={};"
"async function j(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text()||r.status);return r.json();}"
"function esc(v){return String(v==null?'':v).replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]));}"
"function setMsg(t){document.getElementById('msg').textContent=t||'';}"
"function metric(label,value,cls){return `<div class=\"metric\"><div class=\"label\">${label}</div><div class=\"value ${cls||''}\">${value}</div></div>`}"
"function readings(d){let r=d.readings||{},a=[];for(const k of Object.keys(r)){let x=r[k];a.push(`${esc(k)}: ${esc(x.value)}${x.unit?' '+esc(x.unit):''}`)}return a.join('<br>')||'-'}"
"async function load(){const s=await j('/api/status');cfg=s.config;"
"document.getElementById('mdns_hostname').value=cfg.mdns_hostname;"
"document.getElementById('mdns_instance').value=cfg.mdns_instance;"
"document.getElementById('ntp_server').value=cfg.ntp_server;"
"document.getElementById('timezone').value=cfg.timezone;"
"document.getElementById('permit_join_duration_s').value=cfg.permit_join_duration_s;"
"document.getElementById('report_always_on_max_s').value=cfg.report_always_on_max_s;"
"document.getElementById('report_sleepy_max_s').value=cfg.report_sleepy_max_s;"
"document.getElementById('presence_grace_s').value=cfg.presence_grace_s;"
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
"metric('WebSocket',s.services.websocket_client?'conectado':'sin cliente',s.services.websocket_client?'ok':''),"
"metric('TCP consola',s.services.tcp_console_client?'conectada':'sin cliente',s.services.tcp_console_client?'ok':''),"
"metric('Heap libre',s.system.free_heap+' B')"
"].join('');"
"document.getElementById('devices').innerHTML=s.devices.map((d,i)=>`<tr><td>${esc(d.name)}</td><td>${esc(d.ieee)}</td><td class=\"${d.online?'ok':'bad'}\">${d.online?'online':'offline'} / ${esc(d.state)}<br>reporting: ${d.reporting&&d.reporting.configured?'ok':'pendiente'}<br>ep: ${(d.endpoints||[]).length}</td><td>${readings(d)}</td><td>${esc(d.manufacturer||'-')} ${esc(d.model||'')}<br>${esc(d.power_source||'')}</td><td><div class=\"rename\"><input id=\"name_${i}\" value=\"${esc(d.name)}\"><button class=\"small\" onclick=\"renameDev('${d.ieee}',document.getElementById('name_${i}').value)\">Guardar</button></div><div class=\"actions\"><button class=\"small secondary\" onclick=\"devAction('/api/device/reinterview','${d.ieee}')\">Re-entrevistar</button><button class=\"small secondary\" onclick=\"devAction('/api/device/configure','${d.ieee}')\">Reporting</button></div></td></tr>`).join('')||'<tr><td colspan=\"6\">Sin dispositivos</td></tr>';}"
"document.getElementById('save').onclick=async()=>{try{await j('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({mdns_hostname:mdns_hostname.value,mdns_instance:mdns_instance.value,ntp_server:ntp_server.value,timezone:timezone.value,permit_join_duration_s:+permit_join_duration_s.value,report_always_on_max_s:+report_always_on_max_s.value,report_sleepy_max_s:+report_sleepy_max_s.value,presence_grace_s:+presence_grace_s.value})});setMsg('Configuracion guardada');await load();}catch(e){setMsg('Error: '+e.message)}};"
"document.getElementById('refresh').onclick=()=>load().catch(e=>setMsg('Error: '+e.message));"
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
"load().catch(e=>setMsg('Error: '+e.message));setInterval(()=>load().catch(()=>{}),5000);"
"</script></body></html>";

static esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to build JSON");
        return ESP_FAIL;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to serialize JSON");
        return ESP_FAIL;
    }

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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "rebooting");
    esp_err_t err = web_send_json(req, root);
    schedule_delayed_cb(delayed_reboot_cb, "web_reboot");
    return err;
}

static esp_err_t web_post_erase_device_cache_handler(httpd_req_t *req)
{
    nvs_cache_erase();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", "Device cache erased; reboot recommended");
    return web_send_json(req, root);
}

static esp_err_t web_post_zigbee_factory_reset_handler(httpd_req_t *req)
{
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "closed", closed);
    return web_send_json(req, root);
}

static esp_err_t web_post_time_resync_handler(httpd_req_t *req)
{
    (void)req;
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

static void add_reading_value(cJSON *reading, const attr_cache_entry_t *attr)
{
    int len = web_zcl_type_size(attr->attr_type);
    uint32_t raw = web_read_u32_le(attr->value, len);
    int32_t sval = web_signed_value(raw, len);

    switch (attr->cluster_id) {
        case 0x0006:
            cJSON_AddStringToObject(reading, "value", raw ? "ON" : "OFF");
            break;
        case 0x0008:
            cJSON_AddNumberToObject(reading, "value", raw & 0xFFu);
            break;
        case 0x0402:
            cJSON_AddNumberToObject(reading, "value", (double)sval / 100.0);
            break;
        case 0x0405:
            cJSON_AddNumberToObject(reading, "value", (double)(raw & 0xFFFFu) / 100.0);
            break;
        case 0x0403:
            cJSON_AddNumberToObject(reading, "value", (double)sval / 10.0);
            break;
        case 0x0400:
            cJSON_AddNumberToObject(reading, "value", raw & 0xFFFFu);
            break;
        case 0x0406:
            cJSON_AddBoolToObject(reading, "value", (raw & 0x01u) != 0);
            break;
        case 0x0001:
            if (attr->attr_id == 0x0020) {
                cJSON_AddNumberToObject(reading, "value", (raw & 0xFFu) * 100u);
            } else {
                cJSON_AddNumberToObject(reading, "value", (raw & 0xFFu) / 2u);
            }
            break;
        case 0x0B04:
            cJSON_AddNumberToObject(reading, "value", (double)sval / 10.0);
            break;
        case 0x0500:
            cJSON_AddBoolToObject(reading, "value", (raw & 0x01u) == 0);
            break;
        default:
            cJSON_AddNumberToObject(reading, "value", raw);
            break;
    }
}

static void add_device_readings(cJSON *item, const device_record_t *dev)
{
    attr_cache_entry_t attrs[MAX_ATTR_CACHE];
    size_t total = zcl_get_cached_attrs(dev->ieee_addr, attrs, MAX_ATTR_CACHE);
    cJSON *readings = cJSON_AddObjectToObject(item, "readings");
    if (!readings) {
        return;
    }

    size_t count = total > MAX_ATTR_CACHE ? MAX_ATTR_CACHE : total;
    for (size_t i = 0; i < count; i++) {
        ws_attr_meta_t meta;
        if (!ws_model_attr_meta(attrs[i].cluster_id, attrs[i].attr_id, &meta)) {
            continue;
        }

        cJSON *reading = cJSON_CreateObject();
        if (!reading) {
            continue;
        }
        add_reading_value(reading, &attrs[i]);
        if (meta.unit) {
            cJSON_AddStringToObject(reading, "unit", meta.unit);
        }
        cJSON_AddNumberToObject(reading, "endpoint", attrs[i].endpoint_id);
        cJSON_AddNumberToObject(reading, "cluster_id", attrs[i].cluster_id);
        cJSON_AddNumberToObject(reading, "attr_id", attrs[i].attr_id);
        cJSON_AddNumberToObject(reading, "ts", attrs[i].last_update_ms / 1000u);
        cJSON_AddItemToObject(readings, meta.name, reading);
    }
}

static void add_cluster_array(cJSON *parent, const char *name,
                              const uint16_t *clusters, uint8_t count)
{
    cJSON *array = cJSON_AddArrayToObject(parent, name);
    if (!array) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(clusters[i]));
    }
}

static void add_device_endpoints(cJSON *item, const device_record_t *dev)
{
    cJSON *endpoints = cJSON_AddArrayToObject(item, "endpoints");
    if (!endpoints) {
        return;
    }
    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        const endpoint_record_t *ep = &dev->endpoints[i];
        cJSON *ep_json = cJSON_CreateObject();
        if (!ep_json) {
            continue;
        }
        cJSON_AddNumberToObject(ep_json, "id", ep->endpoint_id);
        cJSON_AddNumberToObject(ep_json, "profile_id", ep->profile_id);
        cJSON_AddNumberToObject(ep_json, "device_id", ep->device_id);
        cJSON_AddStringToObject(ep_json, "device_type",
                                utils_device_type_name(ep->device_id));
        add_cluster_array(ep_json, "in_clusters",
                          ep->in_clusters, ep->in_cluster_count);
        add_cluster_array(ep_json, "out_clusters",
                          ep->out_clusters, ep->out_cluster_count);
        cJSON_AddItemToArray(endpoints, ep_json);
    }
}

static esp_err_t web_get_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return web_send_error_json(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "oom", "Unable to allocate JSON");
    }

    json_add_config(root);

    cJSON *system = cJSON_AddObjectToObject(root, "system");
    if (system) {
        cJSON_AddNumberToObject(system, "uptime_s", utils_uptime_ms() / 1000u);
        cJSON_AddNumberToObject(system, "free_heap",
                                (double)esp_get_free_heap_size());
        cJSON_AddNumberToObject(system, "free_internal_heap",
                                (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    eth_driver_status_t eth_status;
    eth_driver_get_status(&eth_status);
    cJSON *network = cJSON_AddObjectToObject(root, "network");
    if (network) {
        cJSON_AddBoolToObject(network, "started", eth_status.started);
        cJSON_AddBoolToObject(network, "link_up", eth_status.link_up);
        cJSON_AddBoolToObject(network, "has_ip", eth_status.has_ip);
        cJSON_AddStringToObject(network, "mac", eth_status.mac);
        cJSON_AddStringToObject(network, "ip", eth_status.ip);
        cJSON_AddStringToObject(network, "netmask", eth_status.netmask);
        cJSON_AddStringToObject(network, "gateway", eth_status.gateway);
    }

    cJSON *services = cJSON_AddObjectToObject(root, "services");
    if (services) {
        cJSON_AddBoolToObject(services, "websocket_client",
                              ws_client_session_is_active());
        cJSON_AddBoolToObject(services, "tcp_console_client",
                              tcp_console_is_connected());
    }

    time_sync_status_t time_status;
    time_sync_get_status(&time_status);
    cJSON *time_json = cJSON_AddObjectToObject(root, "time");
    if (time_json) {
        cJSON_AddBoolToObject(time_json, "started", time_status.started);
        cJSON_AddBoolToObject(time_json, "synced", time_status.synced);
        cJSON_AddStringToObject(time_json, "server", time_status.server);
        cJSON_AddStringToObject(time_json, "timezone", time_status.timezone);
        cJSON_AddNumberToObject(time_json, "last_sync_uptime_s",
                                time_status.last_sync_uptime_s);
    }

    cJSON *join = cJSON_AddObjectToObject(root, "permit_join");
    if (join) {
        cJSON_AddBoolToObject(join, "active", button_handler_permit_join_active());
        cJSON_AddNumberToObject(join, "remaining_s",
                                button_handler_permit_join_remaining_s());
    }

    cJSON *zigbee = cJSON_AddObjectToObject(root, "zigbee");
    bool zb_ready = zigbee_core_is_ready();
    if (zigbee) {
        cJSON_AddBoolToObject(zigbee, "ready", zb_ready);
        if (zb_ready && esp_zb_lock_acquire(pdMS_TO_TICKS(WEB_ZB_LOCK_WAIT_MS))) {
            esp_zb_ieee_addr_t ext_pan = {0};
            esp_zb_ieee_addr_t coord_ieee = {0};
            char ext_pan_str[17];
            char coord_ieee_str[17];
            cJSON_AddNumberToObject(zigbee, "channel", esp_zb_get_current_channel());
            cJSON_AddNumberToObject(zigbee, "pan_id", esp_zb_get_pan_id());
            esp_zb_get_extended_pan_id(ext_pan);
            esp_zb_get_long_address(coord_ieee);
            format_zb_addr(ext_pan, ext_pan_str, sizeof(ext_pan_str));
            format_zb_addr(coord_ieee, coord_ieee_str, sizeof(coord_ieee_str));
            cJSON_AddStringToObject(zigbee, "ext_pan_id", ext_pan_str);
            cJSON_AddStringToObject(zigbee, "coordinator_ieee", coord_ieee_str);
            esp_zb_lock_release();
        }
    }

    cJSON_AddNumberToObject(root, "device_capacity", MAX_DEVICES);
    cJSON_AddNumberToObject(root, "device_count", dm_count());

    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    uint8_t online_count = 0;
    dm_lock();
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *dev = dm_get_by_index(i);
        if (!dev) {
            continue;
        }
        if (dev->online) {
            online_count++;
        }
        if (!devices) {
            continue;
        }

        char ieee[20];
        utils_ieee_to_str(dev->ieee_addr, ieee, sizeof(ieee));
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "ieee", ieee);
        cJSON_AddStringToObject(item, "name", dm_display_name(dev));
        cJSON_AddBoolToObject(item, "online", dev->online);
        cJSON_AddBoolToObject(item, "is_sleepy", dev->is_sleepy);
        cJSON_AddStringToObject(item, "state",
                                utils_device_state_name((int)dev->state));
        cJSON_AddStringToObject(item, "manufacturer", dev->manufacturer);
        cJSON_AddStringToObject(item, "model", dev->model);
        cJSON_AddStringToObject(item, "power_source",
                                utils_power_source_name(dev->power_source));
        cJSON_AddNumberToObject(item, "last_seen_s", dev->last_seen_ms / 1000u);
        cJSON *reporting = cJSON_AddObjectToObject(item, "reporting");
        if (reporting) {
            cJSON_AddBoolToObject(reporting, "configured",
                                  dev->reporting_configured);
            cJSON_AddBoolToObject(reporting, "in_progress",
                                  dev->report_cfg_in_progress);
            cJSON_AddNumberToObject(reporting, "expected",
                                    dev->report_cfg_expected);
            cJSON_AddNumberToObject(reporting, "received",
                                    dev->report_cfg_received);
            cJSON_AddNumberToObject(reporting, "failed",
                                    dev->report_cfg_failed);
        }
        cJSON *stats = cJSON_AddObjectToObject(item, "stats");
        if (stats) {
            cJSON_AddNumberToObject(stats, "report_attr_ok", dev->report_attr_ok);
            cJSON_AddNumberToObject(stats, "report_attr_unchanged",
                                    dev->report_attr_unchanged);
            cJSON_AddNumberToObject(stats, "read_rsp_ok", dev->read_rsp_ok);
            cJSON_AddNumberToObject(stats, "read_rsp_fail", dev->read_rsp_fail);
            cJSON_AddNumberToObject(stats, "interview_attempts",
                                    dev->interview_attempts);
        }
        if (dev->radio_metrics_valid) {
            cJSON_AddNumberToObject(item, "lqi", dev->last_lqi);
            cJSON_AddNumberToObject(item, "rssi", dev->last_rssi);
        }
        add_device_endpoints(item, dev);
        add_device_readings(item, dev);
        cJSON_AddItemToArray(devices, item);
    }
    dm_unlock();
    cJSON_AddNumberToObject(root, "online_devices", online_count);

    return web_send_json(req, root);
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
    config.max_uri_handlers = 24;

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
