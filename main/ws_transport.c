#include "ws_transport.h"

#include "eth_driver.h"
#include "utils.h"
#include "ws_client_session.h"
#include "ws_protocol.h"

#include "esp_http_server.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define WS_URI             "/ws"
#define WS_HTTP_PORT       8080
#define WS_TX_QUEUE_LEN    16
#define WS_TASK_STACK      6144
#define WS_TASK_PRIORITY   4
#define WS_RX_MAX_MESSAGE  1024
#define WS_REFRESH_DELAY_MS 200

#define WS_NOTIFY_AUTONOMOUS_SYNC BIT0
#define WS_NOTIFY_INVENTORY_REFRESH BIT1

typedef struct {
    char payload[WS_PROTOCOL_MAX_MESSAGE];
} ws_tx_msg_t;

static QueueHandle_t s_tx_queue;
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

static void purge_tx_queue(void)
{
    if (s_tx_queue) {
        xQueueReset(s_tx_queue);
    }
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

    esp_err_t err = httpd_ws_send_data(session.server, session.sockfd, &frame);
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

static bool enqueue_payload(const char *payload, void *ctx)
{
    (void)ctx;

    if (!payload || payload[0] == '\0') {
        return false;
    }

    ws_tx_msg_t msg = {0};
    size_t len = strnlen(payload, sizeof(msg.payload));
    if (len >= sizeof(msg.payload)) {
        ZB_LOG("WS TX drop oversized payload");
        return false;
    }

    memcpy(msg.payload, payload, len + 1);
    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ZB_LOG("WS TX queue full, drop message");
        return false;
    }
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

    while (xQueueReceive(s_tx_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
        send_payload_now(msg.payload, NULL);
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
            ZB_LOG("WS client close frame fd=%d", sockfd);
        }
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    if (frame.len >= WS_RX_MAX_MESSAGE) {
        ZB_LOG("WS RX drop oversized frame len=%u", (unsigned)frame.len);
        ws_protocol_send_error(enqueue_payload, NULL, 0, "malformed_message",
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
    ws_protocol_handle_text(buf, enqueue_payload, NULL);
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
        }

        purge_tx_queue();
        clear_inventory_refresh();
        ws_client_session_open(s_server, sockfd);
        ZB_LOG("WS client connected fd=%d mode=autonomous_debug_stream", sockfd);
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
    }
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WS_HTTP_PORT;
    config.ctrl_port = WS_HTTP_PORT + 1;
    config.lru_purge_enable = true;
    config.close_fn = ws_close_fn;

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
    s_tx_queue = xQueueCreate(WS_TX_QUEUE_LEN, sizeof(ws_tx_msg_t));
    configASSERT(s_tx_queue);
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
    return ws_protocol_send_zigbee_event(enqueue_payload, NULL, evt);
}

bool ws_transport_notify_event(const zb_event_t *evt)
{
    if (!ws_client_session_is_active()) {
        return false;
    }
    return ws_protocol_send_zigbee_event(enqueue_payload, NULL, evt);
}

bool ws_transport_send_cmd_result(uint32_t reply_to, const char *status,
                                  bool applied, const char *error_code)
{
    if (!ws_client_session_is_active()) {
        return false;
    }
    return ws_protocol_send_cmd_result(enqueue_payload, NULL, reply_to,
                                       status, applied, error_code);
}
