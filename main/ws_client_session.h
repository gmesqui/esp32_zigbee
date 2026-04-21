#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

typedef struct {
    bool active;
    httpd_handle_t server;
    int sockfd;
    uint32_t session_id;
    uint32_t next_msg_id;
    uint32_t generation;
} ws_client_session_snapshot_t;

void ws_client_session_init(void);
void ws_client_session_open(httpd_handle_t server, int sockfd);
bool ws_client_session_close(int sockfd);
bool ws_client_session_is_active(void);
bool ws_client_session_matches(int sockfd);
uint32_t ws_client_session_next_msg_id(void);
uint32_t ws_client_session_generation(void);
void ws_client_session_snapshot(ws_client_session_snapshot_t *out);
