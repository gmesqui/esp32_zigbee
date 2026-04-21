#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zb_events.h"

#define WS_PROTOCOL_VERSION       1
#define WS_PROTOCOL_MAX_MESSAGE   768
#define WS_PROTOCOL_ITEM_BUFFER   384
#define WS_PROTOCOL_CHUNK_MARGIN  96

typedef bool (*ws_protocol_send_fn_t)(const char *payload, void *ctx);

bool ws_protocol_send_hello_ack(ws_protocol_send_fn_t send_fn, void *ctx,
                                uint32_t reply_to);
bool ws_protocol_send_inventory_stream(ws_protocol_send_fn_t send_fn, void *ctx);
bool ws_protocol_send_state_stream(ws_protocol_send_fn_t send_fn, void *ctx);
bool ws_protocol_send_bootstrap(ws_protocol_send_fn_t send_fn, void *ctx);
bool ws_protocol_send_zigbee_event(ws_protocol_send_fn_t send_fn, void *ctx,
                                   const zb_event_t *evt);
bool ws_protocol_send_cmd_result(ws_protocol_send_fn_t send_fn, void *ctx,
                                 uint32_t reply_to, const char *status,
                                 bool applied, const char *error_code);
bool ws_protocol_send_error(ws_protocol_send_fn_t send_fn, void *ctx,
                            uint32_t reply_to, const char *code,
                            const char *message);
void ws_protocol_handle_text(const char *payload,
                             ws_protocol_send_fn_t send_fn, void *ctx);
