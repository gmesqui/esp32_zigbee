#include "ws_transport.h"

#include "eth_driver.h"
#include "utils.h"
#include "ws_client_session.h"
#include "ws_protocol.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
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

#define WS_NOTIFY_AUTONOMOUS_SYNC BIT0
#define WS_NOTIFY_INVENTORY_REFRESH BIT1

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

    if (!s_tx_lock || xSemaphoreTake(s_tx_lock, 0) != pdTRUE) {
        tx_record_drop_unlocked(priority);
        ZB_LOG("WS TX queue busy, drop priority=%s type=%s",
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

    char buf[WS_RX_MAX_MESSAGE];
    memset(buf, 0, sizeof(buf));
    frame.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    buf[frame.len] = '\0';
    ZB_LOG("WS RX text frame bytes=%u", (unsigned)frame.len);
    ws_protocol_handle_text(buf, enqueue_payload_auto, NULL);
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

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WS_HTTP_PORT;
    config.ctrl_port = WS_HTTP_PORT + 1;
    config.lru_purge_enable = true;
    config.close_fn = ws_close_fn;
    config.send_wait_timeout = WS_SEND_WAIT_TIMEOUT_S;
    config.recv_wait_timeout = WS_RECV_WAIT_TIMEOUT_S;

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
    return httpd_register_uri_handler(s_server, &ws_uri);
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
