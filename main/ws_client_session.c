#include "ws_client_session.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include <string.h>

static SemaphoreHandle_t s_session_mutex;
static ws_client_session_snapshot_t s_sessions[WS_CLIENT_SESSION_MAX];
static uint32_t s_next_msg_id = 1;
static uint32_t s_generation;

void ws_client_session_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_next_msg_id = 1;
    s_generation = 0;
    s_session_mutex = xSemaphoreCreateMutex();
    configASSERT(s_session_mutex);
}

bool ws_client_session_open(httpd_handle_t server, int sockfd)
{
    bool opened = false;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
        if (s_sessions[i].active && s_sessions[i].sockfd == sockfd) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
            if (!s_sessions[i].active) {
                slot = i;
                break;
            }
        }
    }

    if (slot >= 0) {
        ws_client_session_snapshot_t *session = &s_sessions[slot];
        memset(session, 0, sizeof(*session));
        session->active = true;
        session->server = server;
        session->sockfd = sockfd;
        session->session_id = esp_random();
        if (session->session_id == 0) {
            session->session_id = 1;
        }
        session->next_msg_id = 1;
        s_generation++;
        if (s_generation == 0) {
            s_generation = 1;
        }
        session->generation = s_generation;
        opened = true;
    }

    xSemaphoreGive(s_session_mutex);
    return opened;
}

bool ws_client_session_close(int sockfd)
{
    bool closed = false;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
        if (s_sessions[i].active && s_sessions[i].sockfd == sockfd) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            closed = true;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return closed;
}

bool ws_client_session_is_active(void)
{
    bool active = false;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
        if (s_sessions[i].active) {
            active = true;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return active;
}

bool ws_client_session_matches(int sockfd)
{
    bool matches = false;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
        if (s_sessions[i].active && s_sessions[i].sockfd == sockfd) {
            matches = true;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return matches;
}

uint32_t ws_client_session_next_msg_id(void)
{
    uint32_t msg_id;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    msg_id = s_next_msg_id++;
    if (s_next_msg_id == 0) {
        s_next_msg_id = 1;
    }
    xSemaphoreGive(s_session_mutex);
    return msg_id;
}

uint32_t ws_client_session_generation(void)
{
    uint32_t generation;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    generation = s_generation;
    xSemaphoreGive(s_session_mutex);
    return generation;
}

void ws_client_session_snapshot(ws_client_session_snapshot_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < WS_CLIENT_SESSION_MAX; i++) {
        if (s_sessions[i].active) {
            *out = s_sessions[i];
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
}

size_t ws_client_session_collect(ws_client_session_snapshot_t *out,
                                 size_t out_len)
{
    size_t count = 0;

    if (!out || out_len == 0) {
        return 0;
    }

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_CLIENT_SESSION_MAX && count < out_len; i++) {
        if (s_sessions[i].active) {
            out[count++] = s_sessions[i];
        }
    }
    xSemaphoreGive(s_session_mutex);
    return count;
}
