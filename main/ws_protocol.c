#include "ws_protocol.h"

#include "client_actions.h"
#include "device_manager.h"
#include "utils.h"
#include "ws_client_session.h"
#include "ws_model.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    WS_RX_OK = 0,
    WS_RX_MALFORMED,
    WS_RX_UNSUPPORTED_PROTOCOL,
    WS_RX_UNSUPPORTED_COMMAND,
    WS_RX_UNKNOWN_DEVICE,
    WS_RX_BUSY,
    WS_RX_INTERNAL_ERROR,
} ws_rx_status_t;

typedef struct {
    const char *type;
    uint32_t msg_id;
    bool require_ack;
    cJSON *data;
} ws_incoming_msg_t;

typedef bool (*ws_device_item_writer_t)(char *buf, size_t buf_len,
                                        const device_record_t *dev);

static uint32_t next_msg_id(void)
{
    return ws_client_session_next_msg_id();
}

static uint32_t protocol_ts(void)
{
    return utils_uptime_ms() / 1000u;
}

static size_t json_len(const char *buf)
{
    return strnlen(buf, WS_PROTOCOL_MAX_MESSAGE);
}

static bool json_overflowed(const char *buf, const char *p, const char *end)
{
    return p >= end || json_len(buf) >= (WS_PROTOCOL_MAX_MESSAGE - 1u);
}

static void append_envelope_start(char **p, char *end, const char *type,
                                  uint32_t msg_id, uint32_t reply_to)
{
    ws_json_append(p, end, "{\"type\":");
    ws_json_append_string(p, end, type);
    ws_json_append(p, end, ",\"msg_id\":%lu", (unsigned long)msg_id);
    if (reply_to != 0) {
        ws_json_append(p, end, ",\"reply_to\":%lu", (unsigned long)reply_to);
    }
    ws_json_append(p, end, ",\"ts\":%lu,\"data\":", (unsigned long)protocol_ts());
}

static bool send_text(ws_protocol_send_fn_t send_fn, void *ctx, char *buf)
{
    if (!send_fn || !buf || buf[0] == '\0') {
        return false;
    }

    char type[24] = "?";
    const char *type_pos = strstr(buf, "\"type\":\"");
    if (type_pos) {
        type_pos += strlen("\"type\":\"");
        const char *type_end = strchr(type_pos, '"');
        if (type_end) {
            size_t len = (size_t)(type_end - type_pos);
            if (len >= sizeof(type)) {
                len = sizeof(type) - 1u;
            }
            memcpy(type, type_pos, len);
            type[len] = '\0';
        }
    }
    ZB_LOG("WS TX type=%s bytes=%u", type, (unsigned)strlen(buf));
    return send_fn(buf, ctx);
}

static const char *rx_status_code(ws_rx_status_t status)
{
    switch (status) {
        case WS_RX_OK:
            return NULL;
        case WS_RX_MALFORMED:
            return "malformed_message";
        case WS_RX_UNSUPPORTED_PROTOCOL:
            return "unsupported_protocol_version";
        case WS_RX_UNSUPPORTED_COMMAND:
            return "unsupported_command";
        case WS_RX_UNKNOWN_DEVICE:
            return "unknown_device";
        case WS_RX_BUSY:
            return "busy";
        case WS_RX_INTERNAL_ERROR:
        default:
            return "internal_error";
    }
}

static const char *action_error_code(client_action_result_t result)
{
    switch (result) {
        case CLIENT_ACTION_OK:
            return NULL;
        case CLIENT_ACTION_INVALID_ARG:
            return "malformed_message";
        case CLIENT_ACTION_DEVICE_NOT_FOUND:
            return "unknown_device";
        case CLIENT_ACTION_UNSUPPORTED:
            return "unsupported_command";
        case CLIENT_ACTION_BUSY:
            return "busy";
        default:
            return "internal_error";
    }
}

static ws_rx_status_t action_to_rx_status(client_action_result_t result)
{
    switch (result) {
        case CLIENT_ACTION_OK:
            return WS_RX_OK;
        case CLIENT_ACTION_INVALID_ARG:
            return WS_RX_MALFORMED;
        case CLIENT_ACTION_DEVICE_NOT_FOUND:
            return WS_RX_UNKNOWN_DEVICE;
        case CLIENT_ACTION_UNSUPPORTED:
            return WS_RX_UNSUPPORTED_COMMAND;
        case CLIENT_ACTION_BUSY:
            return WS_RX_BUSY;
        default:
            return WS_RX_INTERNAL_ERROR;
    }
}

static bool has_interviewed_device_from(uint8_t start_idx, uint8_t *first_idx)
{
    for (uint8_t idx = start_idx; idx < MAX_DEVICES; idx++) {
        device_record_t *dev = dm_get_by_index(idx);
        if (dev && dev->state >= DEV_STATE_INTERVIEWED) {
            if (first_idx) {
                *first_idx = idx;
            }
            return true;
        }
    }
    return false;
}

static void build_chunk_prefix(char *buf, size_t buf_len, const char *type,
                               const char *stream_id, uint32_t generation,
                               uint32_t chunk_idx)
{
    char *p = buf;
    char *end = buf + buf_len;

    buf[0] = '\0';
    append_envelope_start(&p, end, type, next_msg_id(), 0);
    ws_json_append(&p, end, "{\"stream_id\":");
    ws_json_append_string(&p, end, stream_id);
    ws_json_append(&p, end,
                   ",\"generation\":%lu,\"index\":%lu,\"final\":",
                   (unsigned long)generation, (unsigned long)chunk_idx);
}

static void build_chunk(char *dst, size_t dst_len, const char *prefix,
                        bool final, const char *items)
{
    char *p = dst;
    char *end = dst + dst_len;

    dst[0] = '\0';
    ws_json_append(&p, end, "%s%s,\"devices\":[%s]}}",
                   prefix, final ? "true" : "false", items ? items : "");
}

static bool write_inventory_item(char *buf, size_t buf_len,
                                 const device_record_t *dev)
{
    char *p = buf;
    char *end = buf + buf_len;

    buf[0] = '\0';
    ws_model_append_inventory_device(&p, end, dev);
    return !json_overflowed(buf, p, end);
}

static bool write_state_item(char *buf, size_t buf_len,
                             const device_record_t *dev)
{
    char *p = buf;
    char *end = buf + buf_len;

    buf[0] = '\0';
    ws_model_append_state_device(&p, end, dev);
    return !json_overflowed(buf, p, end);
}

static bool emit_device_stream(const char *message_type, const char *stream_prefix,
                               ws_protocol_send_fn_t send_fn, void *ctx,
                               ws_device_item_writer_t write_item)
{
    uint32_t chunk_idx = 0;
    uint32_t item_count = 0;
    uint32_t generation = ws_client_session_generation();
    char stream_id[24];
    bool ok = true;
    uint8_t next_idx = 0;

    snprintf(stream_id, sizeof(stream_id), "%s-%08lX",
             stream_prefix, (unsigned long)generation);

    ZB_LOG("WS stream %s begin generation=%lu stream_id=%s max_msg=%u item_buf=%u",
           message_type, (unsigned long)generation, stream_id,
           (unsigned)WS_PROTOCOL_MAX_MESSAGE,
           (unsigned)WS_PROTOCOL_ITEM_BUFFER);

    dm_lock();
    while (true) {
        uint8_t idx = 0;
        if (!has_interviewed_device_from(next_idx, &idx)) {
            char prefix[192];
            char chunk[WS_PROTOCOL_MAX_MESSAGE];
            build_chunk_prefix(prefix, sizeof(prefix), message_type, stream_id,
                               generation, chunk_idx);
            build_chunk(chunk, sizeof(chunk), prefix, true, "");
            dm_unlock();
            ZB_LOG("WS stream %s chunk=%lu bytes=%u items=0 final=1",
                   message_type, (unsigned long)chunk_idx,
                   (unsigned)json_len(chunk));
            ok = send_text(send_fn, ctx, chunk) && ok;
            ZB_LOG("WS stream %s end chunks=%lu items=%lu ok=%u",
                   message_type, (unsigned long)(chunk_idx + 1u),
                   (unsigned long)item_count, ok ? 1u : 0u);
            return ok;
        }

        char prefix[192];
        char items[WS_PROTOCOL_MAX_MESSAGE];
        char chunk[WS_PROTOCOL_MAX_MESSAGE];
        size_t item_len = 0;
        uint32_t chunk_items = 0;

        build_chunk_prefix(prefix, sizeof(prefix), message_type, stream_id,
                           generation, chunk_idx);
        items[0] = '\0';

        for (; idx < MAX_DEVICES; idx++) {
            device_record_t *dev = dm_get_by_index(idx);
            if (!dev || dev->state < DEV_STATE_INTERVIEWED) {
                continue;
            }

            char item[WS_PROTOCOL_ITEM_BUFFER];
            if (!write_item(item, sizeof(item), dev)) {
                char id[20];
                utils_ieee_to_str(dev->ieee_addr, id, sizeof(id));
                ZB_LOG("WS stream %s skip oversized item device=%s item_buf=%u",
                       message_type, id, (unsigned)sizeof(item));
                next_idx = (uint8_t)(idx + 1u);
                continue;
            }

            item_len = strlen(item);
            size_t needed = strlen(prefix) + strlen(items) + item_len +
                            (chunk_items > 0 ? 1u : 0u) +
                            strlen("false,\"devices\":[]}}") +
                            WS_PROTOCOL_CHUNK_MARGIN;

            if (chunk_items > 0 && needed >= WS_PROTOCOL_MAX_MESSAGE) {
                break;
            }

            if (chunk_items > 0) {
                strncat(items, ",", sizeof(items) - strlen(items) - 1u);
            }
            strncat(items, item, sizeof(items) - strlen(items) - 1u);
            chunk_items++;
            item_count++;
            next_idx = (uint8_t)(idx + 1u);
        }

        bool final = !has_interviewed_device_from(next_idx, NULL);
        build_chunk(chunk, sizeof(chunk), prefix, final, items);
        dm_unlock();

        size_t chunk_len = json_len(chunk);
        ZB_LOG("WS stream %s chunk=%lu bytes=%u items=%lu final=%u",
               message_type, (unsigned long)chunk_idx, (unsigned)chunk_len,
               (unsigned long)chunk_items, final ? 1u : 0u);

        if (chunk_len >= WS_PROTOCOL_MAX_MESSAGE - 1u) {
            ZB_LOG("WS stream %s chunk overflow, aborting", message_type);
            return false;
        }

        ok = send_text(send_fn, ctx, chunk) && ok;

        if (final) {
            ZB_LOG("WS stream %s end chunks=%lu items=%lu ok=%u",
                   message_type, (unsigned long)(chunk_idx + 1u),
                   (unsigned long)item_count, ok ? 1u : 0u);
            return ok;
        }

        chunk_idx++;
        dm_lock();
    }
}

bool ws_protocol_send_hello_ack(ws_protocol_send_fn_t send_fn, void *ctx,
                                uint32_t reply_to)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);
    ws_client_session_snapshot_t session;

    ws_client_session_snapshot(&session);
    buf[0] = '\0';

    append_envelope_start(&p, end, "hello_ack", next_msg_id(), reply_to);
    ws_json_append(&p, end, "{\"protocol_version\":%d,"
                   "\"server\":\"esp32-zigbee-gateway\","
                   "\"server_version\":\"0.1.0\","
                   "\"gateway_id\":\"esp32-zigbee\","
                   "\"session_id\":\"%08lX\","
                   "\"features\":[\"inventory_stream\",\"state_stream\","
                   "\"commands\",\"ack\"]}}",
                   WS_PROTOCOL_VERSION,
                   (unsigned long)session.session_id);
    return send_text(send_fn, ctx, buf);
}

bool ws_protocol_send_inventory_stream(ws_protocol_send_fn_t send_fn, void *ctx)
{
    return emit_device_stream("inventory_chunk", "inv", send_fn, ctx,
                              write_inventory_item);
}

bool ws_protocol_send_state_stream(ws_protocol_send_fn_t send_fn, void *ctx)
{
    return emit_device_stream("state_chunk", "state", send_fn, ctx,
                              write_state_item);
}

bool ws_protocol_send_bootstrap(ws_protocol_send_fn_t send_fn, void *ctx)
{
    ZB_LOG("WS autonomous sync: sending hello_ack");
    bool ok = ws_protocol_send_hello_ack(send_fn, ctx, 0);

    ZB_LOG("WS autonomous sync: sending fragmented inventory");
    ok = ws_protocol_send_inventory_stream(send_fn, ctx) && ok;

    ZB_LOG("WS autonomous sync: sending initial state snapshot");
    ok = ws_protocol_send_state_stream(send_fn, ctx) && ok;
    return ok;
}

bool ws_protocol_send_zigbee_event(ws_protocol_send_fn_t send_fn, void *ctx,
                                   const zb_event_t *evt)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);
    const char *type = "event";

    if (!evt) {
        return false;
    }

    if (evt->type == ZB_EVT_DEVICE_JOINED) {
        type = "device_joined";
    } else if (evt->type == ZB_EVT_DEVICE_LEAVE) {
        type = "device_left";
    } else if (evt->type == ZB_EVT_DEVICE_UPDATED) {
        type = "device_updated";
    } else if (evt->type == ZB_EVT_INTERVIEW) {
        type = "device_updated";
    } else if (evt->type == ZB_EVT_ATTR_CHANGED) {
        type = "event";
    }

    buf[0] = '\0';
    append_envelope_start(&p, end, type, next_msg_id(), 0);
    ws_json_append(&p, end, "{");
    if (evt->ieee != 0) {
        ws_json_append(&p, end, "\"device_id\":");
        ws_model_append_device_id(&p, end, evt->ieee);
    } else {
        ws_json_append(&p, end, "\"device_id\":null");
    }

    switch (evt->type) {
        case ZB_EVT_ATTR_CHANGED:
            ws_json_append(&p, end, ",\"changes\":");
            ws_model_append_event_change(&p, end, evt);
            break;
        case ZB_EVT_AVAILABILITY:
            ws_json_append(&p, end,
                           ",\"changes\":{\"reachable\":{\"value\":%s,"
                           "\"ts\":%lu,\"quality\":\"valid\"}}",
                           evt->online ? "true" : "false",
                           (unsigned long)protocol_ts());
            break;
        case ZB_EVT_PERMIT_JOIN:
            ws_json_append(&p, end,
                           ",\"event\":\"permit_join\",\"value\":%s,"
                           "\"duration\":%u",
                           evt->permit_join_duration > 0 ? "true" : "false",
                           evt->permit_join_duration);
            break;
        case ZB_EVT_INTERVIEW:
            ws_json_append(&p, end, ",\"status\":");
            ws_json_append_string(&p, end,
                                  evt->interview_status ? evt->interview_status
                                                        : "unknown");
            break;
        default:
            ws_json_append(&p, end, ",\"name\":");
            ws_json_append_string(&p, end, evt->friendly_name);
            break;
    }
    ws_json_append(&p, end, "}}");
    return send_text(send_fn, ctx, buf);
}

bool ws_protocol_send_cmd_result(ws_protocol_send_fn_t send_fn, void *ctx,
                                 uint32_t reply_to, const char *status,
                                 bool applied, const char *error_code)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);

    buf[0] = '\0';
    append_envelope_start(&p, end, "cmd_result", next_msg_id(), reply_to);
    ws_json_append(&p, end, "{\"status\":");
    ws_json_append_string(&p, end, status ? status : "error");
    ws_json_append(&p, end, ",\"applied\":%s", applied ? "true" : "false");
    if (error_code && error_code[0]) {
        ws_json_append(&p, end, ",\"error\":");
        ws_json_append_string(&p, end, error_code);
    }
    ws_json_append(&p, end, "}}");
    return send_text(send_fn, ctx, buf);
}

bool ws_protocol_send_error(ws_protocol_send_fn_t send_fn, void *ctx,
                            uint32_t reply_to, const char *code,
                            const char *message)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);

    buf[0] = '\0';
    append_envelope_start(&p, end, "error", next_msg_id(), reply_to);
    ws_json_append(&p, end, "{\"code\":");
    ws_json_append_string(&p, end, code ? code : "internal_error");
    ws_json_append(&p, end, ",\"message\":");
    ws_json_append_string(&p, end, message ? message : "Internal error");
    ws_json_append(&p, end, "}}");
    ZB_LOG("WS protocol error reply_to=%lu code=%s",
           (unsigned long)reply_to, code ? code : "internal_error");
    return send_text(send_fn, ctx, buf);
}

static void send_ack(ws_protocol_send_fn_t send_fn, void *ctx, uint32_t reply_to)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);

    buf[0] = '\0';
    append_envelope_start(&p, end, "ack", next_msg_id(), reply_to);
    ws_json_append(&p, end, "{\"status\":\"ok\"}}");
    send_text(send_fn, ctx, buf);
}

static void send_pong(ws_protocol_send_fn_t send_fn, void *ctx, uint32_t reply_to)
{
    char buf[WS_PROTOCOL_MAX_MESSAGE];
    char *p = buf;
    char *end = buf + sizeof(buf);

    buf[0] = '\0';
    append_envelope_start(&p, end, "pong", next_msg_id(), reply_to);
    ws_json_append(&p, end, "{}}");
    send_text(send_fn, ctx, buf);
}

static bool validate_incoming_message(cJSON *root, ws_incoming_msg_t *out)
{
    if (!root || !out || !cJSON_IsObject(root)) {
        return false;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *msg_id = cJSON_GetObjectItemCaseSensitive(root, "msg_id");
    cJSON *require_ack = cJSON_GetObjectItemCaseSensitive(root, "require_ack");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!cJSON_IsString(type) || !type->valuestring ||
        !cJSON_IsNumber(msg_id) || msg_id->valuedouble < 1 ||
        !cJSON_IsObject(data)) {
        return false;
    }

    out->type = type->valuestring;
    out->msg_id = (uint32_t)msg_id->valuedouble;
    out->require_ack = cJSON_IsTrue(require_ack);
    out->data = data;
    return true;
}

static ws_rx_status_t validate_hello_data(cJSON *data)
{
    cJSON *protocol_version = cJSON_GetObjectItemCaseSensitive(data,
                                                               "protocol_version");
    if (protocol_version) {
        if (!cJSON_IsNumber(protocol_version)) {
            return WS_RX_MALFORMED;
        }
        if (protocol_version->valueint != WS_PROTOCOL_VERSION) {
            return WS_RX_UNSUPPORTED_PROTOCOL;
        }
    }
    return WS_RX_OK;
}

static ws_rx_status_t execute_cmd_data(cJSON *data,
                                       client_action_result_t *action_out)
{
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(data, "device_id");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(data, "command");
    cJSON *cluster = cJSON_GetObjectItemCaseSensitive(data, "cluster");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(data, "params");

    if (action_out) {
        *action_out = CLIENT_ACTION_INVALID_ARG;
    }

    if (!cJSON_IsString(device_id) || !device_id->valuestring ||
        !cJSON_IsString(command) || !command->valuestring ||
        !cJSON_IsString(cluster) || !cluster->valuestring ||
        !cJSON_IsObject(params)) {
        return WS_RX_MALFORMED;
    }

    if (strcmp(command->valuestring, "set") == 0) {
        if (strcmp(cluster->valuestring, "onoff") == 0) {
            cJSON *state = cJSON_GetObjectItemCaseSensitive(params, "state");
            if (!cJSON_IsBool(state)) {
                return WS_RX_MALFORMED;
            }
            client_action_result_t result =
                client_actions_set_state(device_id->valuestring,
                                         cJSON_IsTrue(state) ? "ON" : "OFF",
                                         0);
            if (action_out) {
                *action_out = result;
            }
            return action_to_rx_status(result);
        }
    }

    return WS_RX_UNSUPPORTED_COMMAND;
}

void ws_protocol_handle_text(const char *payload,
                             ws_protocol_send_fn_t send_fn, void *ctx)
{
    if (!payload || payload[0] == '\0') {
        ws_protocol_send_error(send_fn, ctx, 0, "malformed_message",
                               "Empty WebSocket message");
        return;
    }

    size_t payload_len = strnlen(payload, WS_PROTOCOL_MAX_MESSAGE + 1u);
    if (payload_len > WS_PROTOCOL_MAX_MESSAGE) {
        ws_protocol_send_error(send_fn, ctx, 0, "malformed_message",
                               "Message exceeds protocol size limit");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        ws_protocol_send_error(send_fn, ctx, 0, "malformed_message",
                               "Invalid JSON object");
        return;
    }

    ws_incoming_msg_t msg = {0};
    if (!validate_incoming_message(root, &msg)) {
        cJSON *msg_id_item = cJSON_GetObjectItemCaseSensitive(root, "msg_id");
        uint32_t reply_to = cJSON_IsNumber(msg_id_item)
            ? (uint32_t)msg_id_item->valuedouble
            : 0;

        ws_protocol_send_error(send_fn, ctx, reply_to, "malformed_message",
                               "Missing or invalid common message fields");
        cJSON_Delete(root);
        return;
    }

    ZB_LOG("WS RX protocol type=%s msg_id=%lu bytes=%u",
           msg.type, (unsigned long)msg.msg_id, (unsigned)payload_len);

    if (msg.require_ack) {
        send_ack(send_fn, ctx, msg.msg_id);
    }

    if (strcmp(msg.type, "hello") == 0) {
        ws_rx_status_t status = validate_hello_data(msg.data);
        if (status == WS_RX_OK) {
            ws_protocol_send_hello_ack(send_fn, ctx, msg.msg_id);
        } else {
            ws_protocol_send_error(send_fn, ctx, msg.msg_id,
                                   rx_status_code(status),
                                   "Unsupported or malformed hello message");
        }
    } else if (strcmp(msg.type, "ping") == 0) {
        send_pong(send_fn, ctx, msg.msg_id);
    } else if (strcmp(msg.type, "pong") == 0) {
        ZB_LOG("WS RX pong msg_id=%lu", (unsigned long)msg.msg_id);
    } else if (strcmp(msg.type, "ack") == 0) {
        ZB_LOG("WS RX ack msg_id=%lu", (unsigned long)msg.msg_id);
    } else if (strcmp(msg.type, "resync") == 0) {
        ZB_LOG("WS RX resync msg_id=%lu", (unsigned long)msg.msg_id);
        ws_protocol_send_inventory_stream(send_fn, ctx);
        ws_protocol_send_state_stream(send_fn, ctx);
    } else if (strcmp(msg.type, "cmd") == 0) {
        client_action_result_t action_result = CLIENT_ACTION_INVALID_ARG;
        ws_rx_status_t status = execute_cmd_data(msg.data, &action_result);
        const char *error = status == WS_RX_OK ? NULL : action_error_code(action_result);
        if (status != WS_RX_OK && !error) {
            error = rx_status_code(status);
        }

        ZB_LOG("WS RX cmd msg_id=%lu status=%s",
               (unsigned long)msg.msg_id, status == WS_RX_OK ? "ok" : error);
        ws_protocol_send_cmd_result(send_fn, ctx, msg.msg_id,
                                    status == WS_RX_OK ? "ok" : "error",
                                    status == WS_RX_OK,
                                    error);
    } else if (strcmp(msg.type, "error") == 0) {
        ZB_LOG("WS RX peer error msg_id=%lu", (unsigned long)msg.msg_id);
    } else {
        ws_protocol_send_error(send_fn, ctx, msg.msg_id,
                               "unsupported_command",
                               "Unsupported message type");
    }

    cJSON_Delete(root);
}
