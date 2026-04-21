#include "ws_client_session.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include <string.h>

static SemaphoreHandle_t s_session_mutex;
static ws_client_session_snapshot_t s_session;

void ws_client_session_init(void)
{
    memset(&s_session, 0, sizeof(s_session));
    s_session_mutex = xSemaphoreCreateMutex();
    configASSERT(s_session_mutex);
}

void ws_client_session_open(httpd_handle_t server, int sockfd)
{
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    s_session.active = true;
    s_session.server = server;
    s_session.sockfd = sockfd;
    s_session.session_id = esp_random();
    if (s_session.session_id == 0) {
        s_session.session_id = 1;
    }
    s_session.next_msg_id = 1;
    s_session.generation++;
    if (s_session.generation == 0) {
        s_session.generation = 1;
    }
    xSemaphoreGive(s_session_mutex);
}

bool ws_client_session_close(int sockfd)
{
    bool closed = false;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_session.active && s_session.sockfd == sockfd) {
        s_session.active = false;
        s_session.server = NULL;
        s_session.sockfd = -1;
        closed = true;
    }
    xSemaphoreGive(s_session_mutex);
    return closed;
}

bool ws_client_session_is_active(void)
{
    bool active;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    active = s_session.active;
    xSemaphoreGive(s_session_mutex);
    return active;
}

bool ws_client_session_matches(int sockfd)
{
    bool matches;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    matches = s_session.active && s_session.sockfd == sockfd;
    xSemaphoreGive(s_session_mutex);
    return matches;
}

uint32_t ws_client_session_next_msg_id(void)
{
    uint32_t msg_id;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    msg_id = s_session.next_msg_id++;
    if (s_session.next_msg_id == 0) {
        s_session.next_msg_id = 1;
    }
    xSemaphoreGive(s_session_mutex);
    return msg_id;
}

uint32_t ws_client_session_generation(void)
{
    uint32_t generation;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    generation = s_session.generation;
    xSemaphoreGive(s_session_mutex);
    return generation;
}

void ws_client_session_snapshot(ws_client_session_snapshot_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    *out = s_session;
    xSemaphoreGive(s_session_mutex);
}
