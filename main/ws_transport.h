#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "zb_events.h"

void ws_transport_init(EventGroupHandle_t eth_ready_eg);

bool ws_transport_notify_inventory(void);
bool ws_transport_notify_state_change(const zb_event_t *evt);
bool ws_transport_notify_event(const zb_event_t *evt);
bool ws_transport_send_cmd_result(uint32_t reply_to, const char *status,
                                  bool applied, const char *error_code);

