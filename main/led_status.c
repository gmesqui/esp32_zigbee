#include "led_status.h"

#include <stdbool.h>
#include <string.h>

#include "driver/rmt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const gpio_num_t LED_GPIO = GPIO_NUM_27;
static const uint32_t LED_STRIP_LEN = 1;
static const rmt_channel_t LED_RMT_CHANNEL = RMT_CHANNEL_0;
static const TickType_t HEARTBEAT_PERIOD_TICKS = pdMS_TO_TICKS(2200);
static const uint8_t HEARTBEAT_WAVE[] = {
    0, 1, 2, 4, 7, 11, 15, 20,
    25, 30, 35, 40, 44, 48, 51, 53,
    54, 53, 51, 48, 44, 40, 35, 30,
    25, 20, 15, 11, 7, 4, 2, 1,
};
static led_strip_t *s_strip = NULL;
static led_base_state_t s_base = LED_BASE_BOOT;
static TickType_t s_pulse_deadline_tick = 0;
static uint8_t s_pulse_r = 0;
static uint8_t s_pulse_g = 0;
static uint8_t s_pulse_b = 0;
static TickType_t s_heartbeat_tick = 0;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static uint8_t sat_add_u8(uint8_t base, uint8_t delta)
{
    const uint16_t sum = (uint16_t)base + delta;
    return (sum > UINT8_MAX) ? UINT8_MAX : (uint8_t)sum;
}

static uint8_t blend_u8(uint8_t a, uint8_t b, uint8_t mix)
{
    const uint16_t inv_mix = (uint16_t)UINT8_MAX - mix;
    const uint16_t blended = (uint16_t)a * inv_mix + (uint16_t)b * mix;
    return (uint8_t)((blended + 127) / UINT8_MAX);
}

static uint8_t heartbeat_wave_mix(TickType_t now)
{
    TickType_t elapsed = now - s_heartbeat_tick;
    const size_t sample_count = sizeof(HEARTBEAT_WAVE) / sizeof(HEARTBEAT_WAVE[0]);

    if (elapsed >= HEARTBEAT_PERIOD_TICKS) {
        elapsed %= HEARTBEAT_PERIOD_TICKS;
        s_heartbeat_tick = now - elapsed;
    }

    size_t idx = ((uint64_t)elapsed * sample_count) / HEARTBEAT_PERIOD_TICKS;
    if (idx >= sample_count) {
        idx = sample_count - 1;
    }
    return (uint8_t)(((uint16_t)HEARTBEAT_WAVE[idx] * UINT8_MAX) / 54);
}

static rgb_t ready_closed_wave_color(TickType_t now)
{
    const rgb_t green = {.r = 0, .g = 26, .b = 0};
    const rgb_t blue = {.r = 0, .g = 2, .b = 30};
    const uint8_t mix = heartbeat_wave_mix(now);

    return (rgb_t){
        .r = blend_u8(green.r, blue.r, mix),
        .g = blend_u8(green.g, blue.g, mix),
        .b = blend_u8(green.b, blue.b, mix),
    };
}

static rgb_t base_color(led_base_state_t state)
{
    switch (state) {
    case LED_BASE_BOOT:
        return (rgb_t){.r = 0, .g = 0, .b = 18};      /* azul tenue */
    case LED_BASE_FORMING:
        return (rgb_t){.r = 0, .g = 18, .b = 18};     /* cian tenue */
    case LED_BASE_READY_CLOSED:
        return (rgb_t){.r = 0, .g = 18, .b = 0};      /* verde tenue */
    case LED_BASE_READY_OPEN:
        return (rgb_t){.r = 18, .g = 10, .b = 0};     /* ambar tenue */
    case LED_BASE_ERROR:
    default:
        return (rgb_t){.r = 18, .g = 0, .b = 0};      /* rojo tenue */
    }
}

static void apply_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return;
    }
    (void)s_strip->set_pixel(s_strip, 0, r, g, b);
    (void)s_strip->refresh(s_strip, 100);
}

void led_status_init(void)
{
    rmt_config_t tx_config = RMT_DEFAULT_CONFIG_TX(LED_GPIO, LED_RMT_CHANNEL);
    tx_config.clk_div = 2;
    tx_config.mem_block_num = 1;
    (void)rmt_config(&tx_config);
    (void)rmt_driver_install(tx_config.channel, 0, 0);

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(LED_STRIP_LEN, (led_strip_dev_t)LED_RMT_CHANNEL);
    s_strip = led_strip_new_rmt_ws2812(&strip_config);
    s_base = LED_BASE_BOOT;
    s_pulse_deadline_tick = 0;
    s_heartbeat_tick = xTaskGetTickCount();
    apply_color(0, 0, 18);
}

void led_status_set_base(led_base_state_t state)
{
    s_base = state;
    led_status_poll();
}

void led_status_pulse(led_pulse_t pulse)
{
    switch (pulse) {
    case LED_PULSE_NET:
        s_pulse_r = 0; s_pulse_g = 0; s_pulse_b = 80;      /* azul */
        break;
    case LED_PULSE_EVENT:
        s_pulse_r = 80; s_pulse_g = 80; s_pulse_b = 80;    /* blanco */
        break;
    case LED_PULSE_INTERVIEW:
    default:
        s_pulse_r = 60; s_pulse_g = 0; s_pulse_b = 70;     /* violeta */
        break;
    }
    s_pulse_deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(120);
    led_status_poll();
}

void led_status_poll(void)
{
    rgb_t b = base_color(s_base);
    const TickType_t now = xTaskGetTickCount();
    if (s_base == LED_BASE_READY_CLOSED) {
        b = ready_closed_wave_color(now);
    } else {
        s_heartbeat_tick = now;
    }

    if ((int32_t)(now - s_pulse_deadline_tick) < 0) {
        uint8_t r = sat_add_u8(b.r, s_pulse_r);
        uint8_t g = sat_add_u8(b.g, s_pulse_g);
        uint8_t bl = sat_add_u8(b.b, s_pulse_b);
        apply_color(r, g, bl);
    } else {
        apply_color(b.r, b.g, b.b);
    }
}
