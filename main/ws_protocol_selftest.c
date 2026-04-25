#include "ws_protocol_selftest.h"

#include "utils.h"
#include "ws_client_session.h"
#include "ws_protocol.h"

#include <stdio.h>
#include <string.h>

#define printf(...) utils_console_printf(__VA_ARGS__)

typedef struct {
    uint16_t count;
    uint16_t errors;
    uint16_t hello_ack;
    uint16_t inventory_chunks;
    uint16_t state_chunks;
    uint16_t event_messages;
    uint16_t cmd_results;
    uint16_t acks;
    uint16_t max_len;
    char last[WS_PROTOCOL_MAX_MESSAGE];
} capture_ctx_t;

static const char *payload_type(const char *payload, char *buf, size_t buf_len)
{
    const char *type_pos = payload ? strstr(payload, "\"type\":\"") : NULL;

    if (!buf || buf_len == 0) {
        return "";
    }
    snprintf(buf, buf_len, "?");
    if (!type_pos) {
        return buf;
    }

    type_pos += strlen("\"type\":\"");
    const char *type_end = strchr(type_pos, '"');
    if (!type_end) {
        return buf;
    }

    size_t len = (size_t)(type_end - type_pos);
    if (len >= buf_len) {
        len = buf_len - 1u;
    }
    memcpy(buf, type_pos, len);
    buf[len] = '\0';
    return buf;
}

static bool capture_send(const char *payload, void *ctx)
{
    capture_ctx_t *cap = (capture_ctx_t *)ctx;
    char type[24];

    if (!cap || !payload) {
        return false;
    }

    size_t len = strnlen(payload, WS_PROTOCOL_MAX_MESSAGE + 1u);
    cap->count++;
    if (len > cap->max_len) {
        cap->max_len = (uint16_t)len;
    }
    if (len >= WS_PROTOCOL_MAX_MESSAGE) {
        return false;
    }

    snprintf(cap->last, sizeof(cap->last), "%s", payload);
    payload_type(payload, type, sizeof(type));

    if (strcmp(type, "error") == 0) cap->errors++;
    else if (strcmp(type, "hello_ack") == 0) cap->hello_ack++;
    else if (strcmp(type, "inventory_chunk") == 0) cap->inventory_chunks++;
    else if (strcmp(type, "state_chunk") == 0) cap->state_chunks++;
    else if (strcmp(type, "event") == 0) cap->event_messages++;
    else if (strcmp(type, "cmd_result") == 0) cap->cmd_results++;
    else if (strcmp(type, "ack") == 0) cap->acks++;

    return true;
}

static void record_check(ws_protocol_selftest_result_t *result,
                         const char *name, bool ok, bool verbose)
{
    if (ok) {
        result->passed++;
    } else {
        result->failed++;
    }

    if (verbose) {
        printf("WS TEST %-34s %s\n", name, ok ? "PASS" : "FAIL");
    }
}

static void record_skip(ws_protocol_selftest_result_t *result,
                        const char *name, const char *reason, bool verbose)
{
    result->skipped++;
    if (verbose) {
        printf("WS TEST %-34s SKIP (%s)\n", name, reason ? reason : "-");
    }
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void test_session_lifecycle(ws_protocol_selftest_result_t *result,
                                   bool verbose)
{
    if (ws_client_session_is_active()) {
        record_skip(result, "session lifecycle", "active client connected", verbose);
        return;
    }

    const int test_fd = -1001;
    ws_client_session_open(NULL, test_fd);

    ws_client_session_snapshot_t snapshot = {0};
    ws_client_session_snapshot(&snapshot);
    bool ok = snapshot.active &&
              snapshot.sockfd == test_fd &&
              ws_client_session_matches(test_fd) &&
              ws_client_session_next_msg_id() == 1 &&
              ws_client_session_next_msg_id() == 2;

    bool closed = ws_client_session_close(test_fd);
    ok = ok && closed && !ws_client_session_is_active();
    record_check(result, "session lifecycle", ok, verbose);
}

static void test_serializers(ws_protocol_selftest_result_t *result, bool verbose)
{
    capture_ctx_t cap = {0};
    bool opened_here = false;

    if (!ws_client_session_is_active()) {
        ws_client_session_open(NULL, -1002);
        opened_here = true;
    }

    bool ok = ws_protocol_send_hello_ack(capture_send, &cap, 0);
    ok = ok && cap.hello_ack == 1 &&
         contains(cap.last, "\"protocol_version\":1") &&
         cap.max_len < WS_PROTOCOL_MAX_MESSAGE;
    record_check(result, "hello_ack serializer", ok, verbose);

    zb_event_t evt = {
        .type = ZB_EVT_ATTR_CHANGED,
        .ieee = 0x00124B00AABBCCDDULL,
        .cluster_id = 0x0006,
        .attr_id = 0x0000,
        .attr_type = 0x10,
        .value = {1},
    };
    snprintf(evt.friendly_name, sizeof(evt.friendly_name), "selftest_switch");

    memset(&cap, 0, sizeof(cap));
    ok = ws_protocol_send_zigbee_event(capture_send, &cap, &evt);
    ok = ok && cap.event_messages == 1 &&
         contains(cap.last, "\"changes\"") &&
         contains(cap.last, "\"state\"") &&
         cap.max_len < WS_PROTOCOL_MAX_MESSAGE;
    record_check(result, "attribute event serializer", ok, verbose);

    if (opened_here) {
        ws_client_session_close(-1002);
    }
}

static void test_inventory_fragmentation(ws_protocol_selftest_result_t *result,
                                        bool verbose)
{
    capture_ctx_t cap = {0};
    bool opened_here = false;

    if (!ws_client_session_is_active()) {
        ws_client_session_open(NULL, -1003);
        opened_here = true;
    }

    bool ok = ws_protocol_send_inventory_stream(capture_send, &cap);
    ok = ok && cap.inventory_chunks >= 1 &&
         cap.max_len < WS_PROTOCOL_MAX_MESSAGE &&
         contains(cap.last, "\"final\":true");
    record_check(result, "inventory chunking", ok, verbose);

    memset(&cap, 0, sizeof(cap));
    ok = ws_protocol_send_state_stream(capture_send, &cap);
    ok = ok && cap.state_chunks >= 1 &&
         cap.max_len < WS_PROTOCOL_MAX_MESSAGE &&
         contains(cap.last, "\"final\":true");
    record_check(result, "state chunking", ok, verbose);

    if (opened_here) {
        ws_client_session_close(-1003);
    }
}

static void test_invalid_incoming(ws_protocol_selftest_result_t *result,
                                  bool verbose)
{
    capture_ctx_t cap = {0};
    bool opened_here = false;

    if (!ws_client_session_is_active()) {
        ws_client_session_open(NULL, -1004);
        opened_here = true;
    }

    ws_protocol_handle_text("{", capture_send, &cap);
    bool ok = cap.errors == 1 && contains(cap.last, "malformed_message");
    record_check(result, "invalid json rejected", ok, verbose);

    memset(&cap, 0, sizeof(cap));
    ws_protocol_handle_text("{\"type\":\"ping\",\"msg_id\":1}", capture_send, &cap);
    ok = cap.errors == 1 && contains(cap.last, "malformed_message");
    record_check(result, "missing data rejected", ok, verbose);

    memset(&cap, 0, sizeof(cap));
    ws_protocol_handle_text("{\"type\":\"unknown\",\"msg_id\":2,\"data\":{}}",
                            capture_send, &cap);
    ok = cap.errors == 1 && contains(cap.last, "unsupported_command");
    record_check(result, "unknown type rejected", ok, verbose);

    memset(&cap, 0, sizeof(cap));
    ws_protocol_handle_text("{\"type\":\"ping\",\"msg_id\":3,\"require_ack\":true,\"data\":{}}",
                            capture_send, &cap);
    ok = cap.acks == 1 && contains(cap.last, "\"type\":\"pong\"");
    record_check(result, "ping ack/pong response", ok, verbose);

    if (opened_here) {
        ws_client_session_close(-1004);
    }
}

ws_protocol_selftest_result_t ws_protocol_selftest_run(bool verbose)
{
    ws_protocol_selftest_result_t result = {0};

    printf("WS protocol self-test start\n");
    test_session_lifecycle(&result, verbose);
    test_serializers(&result, verbose);
    test_inventory_fragmentation(&result, verbose);
    test_invalid_incoming(&result, verbose);
    printf("WS protocol self-test done: passed=%u failed=%u skipped=%u\n",
           result.passed, result.failed, result.skipped);

    return result;
}
