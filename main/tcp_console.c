#include "tcp_console.h"

#include "eth_driver.h"
#include "utils.h"

#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include <errno.h>
#include <string.h>

#define TCP_CONSOLE_TASK_STACK 4096
#define TCP_CONSOLE_TASK_PRIORITY 3
#define TCP_CONSOLE_RX_BUFFER_SIZE 512
#define TCP_CONSOLE_RECV_CHUNK 64
#define TCP_CONSOLE_SEND_WAIT_MS 10

static EventGroupHandle_t s_eth_eg;
static RingbufHandle_t s_rx_ring;
static SemaphoreHandle_t s_client_lock;
static int s_client_fd = -1;

static void client_set(int fd)
{
    if (xSemaphoreTake(s_client_lock, portMAX_DELAY) == pdTRUE) {
        s_client_fd = fd;
        xSemaphoreGive(s_client_lock);
    }
}

static int client_get(void)
{
    int fd = -1;

    if (s_client_lock && xSemaphoreTake(s_client_lock, 0) == pdTRUE) {
        fd = s_client_fd;
        xSemaphoreGive(s_client_lock);
    }

    return fd;
}

static void client_close_current(void)
{
    int fd = -1;

    if (xSemaphoreTake(s_client_lock, portMAX_DELAY) == pdTRUE) {
        fd = s_client_fd;
        s_client_fd = -1;
        xSemaphoreGive(s_client_lock);
    }

    if (fd >= 0) {
        shutdown(fd, 0);
        close(fd);
    }
}

bool tcp_console_is_connected(void)
{
    return client_get() >= 0;
}

size_t tcp_console_write(const char *data, size_t len)
{
    if (!data || len == 0 || !s_client_lock) {
        return 0;
    }

    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(TCP_CONSOLE_SEND_WAIT_MS)) != pdTRUE) {
        return 0;
    }

    int fd = s_client_fd;
    if (fd < 0) {
        xSemaphoreGive(s_client_lock);
        return 0;
    }

    size_t sent_total = 0;
    while (sent_total < len) {
        int sent = send(fd, data + sent_total, len - sent_total, MSG_DONTWAIT);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        }

        s_client_fd = -1;
        shutdown(fd, 0);
        close(fd);
        break;
    }

    xSemaphoreGive(s_client_lock);
    return sent_total;
}

int tcp_console_read_bytes(uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    if (!buf || len == 0 || !s_rx_ring) {
        return 0;
    }

    size_t got_len = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_rx_ring, &got_len,
                                                       ticks_to_wait, len);
    if (!data || got_len == 0) {
        return 0;
    }

    memcpy(buf, data, got_len);
    vRingbufferReturnItem(s_rx_ring, data);
    return (int)got_len;
}

static void tcp_console_task(void *arg)
{
    (void)arg;

    ZB_LOG("TCP console: waiting for Ethernet IP");
    xEventGroupWaitBits(s_eth_eg, ETH_IP_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ZB_LOG("TCP console: socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_CONSOLE_PORT),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ZB_LOG("TCP console: bind port=%u failed errno=%d",
               (unsigned)TCP_CONSOLE_PORT, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 1) != 0) {
        ZB_LOG("TCP console: listen failed errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ZB_LOG("TCP console listening on tcp://<gateway>:%u",
           (unsigned)TCP_CONSOLE_PORT);

    for (;;) {
        struct sockaddr_in source_addr = {0};
        socklen_t addr_len = sizeof(source_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
        if (client_fd < 0) {
            ZB_LOG("TCP console: accept failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct timeval recv_timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &recv_timeout, sizeof(recv_timeout));

        client_close_current();
        client_set(client_fd);

        ZB_LOG("TCP console client connected");
        utils_console_printf("\nTCP console ready on port %u. Press '?' for help.\n",
                             (unsigned)TCP_CONSOLE_PORT);

        uint8_t rx[TCP_CONSOLE_RECV_CHUNK];
        for (;;) {
            int n = recv(client_fd, rx, sizeof(rx), 0);
            if (n > 0) {
                if (xRingbufferSend(s_rx_ring, rx, (size_t)n, 0) != pdTRUE) {
                    utils_console_printf("TCP console RX buffer full, dropped %d byte(s)\n", n);
                }
                continue;
            }

            if (n == 0) {
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }

        client_close_current();
        ZB_LOG("TCP console client disconnected");
    }
}

void tcp_console_init(EventGroupHandle_t eth_ready_eg)
{
    s_eth_eg = eth_ready_eg;
    s_rx_ring = xRingbufferCreate(TCP_CONSOLE_RX_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_client_lock = xSemaphoreCreateMutex();
    configASSERT(s_rx_ring);
    configASSERT(s_client_lock);

    xTaskCreate(tcp_console_task, "tcp_console_task",
                TCP_CONSOLE_TASK_STACK, NULL,
                TCP_CONSOLE_TASK_PRIORITY, NULL);
}
