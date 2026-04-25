#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TCP_CONSOLE_PORT 2323

/** Start the TCP console server once Ethernet has an IP address. */
void tcp_console_init(EventGroupHandle_t eth_ready_eg);

/** Returns true while a TCP console client is connected. */
bool tcp_console_is_connected(void);

/** Write bytes to the connected TCP console client, dropping if it would block. */
size_t tcp_console_write(const char *data, size_t len);

/** Read bytes received from the TCP console client. */
int tcp_console_read_bytes(uint8_t *buf, size_t len, TickType_t ticks_to_wait);
