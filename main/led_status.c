#include "led_status.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const gpio_num_t LED_GPIO = GPIO_NUM_27;
static const uint32_t LED_STRIP_LEN = 1;
static led_strip_handle_t s_strip = NULL;
static led_base_state_t s_base = LED_BASE_BOOT;
static TickType_t s_pulse_deadline_tick = 0;
static uint8_t s_pulse_r = 0;
static uint8_t s_pulse_g = 0;
static uint8_t s_pulse_b = 0;
static TickType_t s_heartbeat_tick = 0;
static bool s_heartbeat_high = false;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

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
    (void)led_strip_set_pixel(s_strip, 0, r, g, b);
    (void)led_strip_refresh(s_strip);
}

void led_status_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_STRIP_LEN,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };
    (void)led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    s_base = LED_BASE_BOOT;
    s_pulse_deadline_tick = 0;
    s_heartbeat_tick = xTaskGetTickCount();
    s_heartbeat_high = false;
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
        const TickType_t period = s_heartbeat_high ? pdMS_TO_TICKS(120) : pdMS_TO_TICKS(1800);
        if ((int32_t)(now - s_heartbeat_tick) >= 0) {
            s_heartbeat_high = !s_heartbeat_high;
            s_heartbeat_tick = now + period;
        }
        if (s_heartbeat_high) {
            b.g = (uint8_t)(b.g + 8); /* respiracion/pulso leve en verde */
        }
    } else {
        s_heartbeat_high = false;
        s_heartbeat_tick = now + pdMS_TO_TICKS(1800);
    }

    if ((int32_t)(now - s_pulse_deadline_tick) < 0) {
        uint8_t r = b.r + s_pulse_r;
        uint8_t g = b.g + s_pulse_g;
        uint8_t bl = b.b + s_pulse_b;
        if (r < b.r) {
            r = 255;
        }
        if (g < b.g) {
            g = 255;
        }
        if (bl < b.b) {
            bl = 255;
        }
        apply_color(r, g, bl);
    } else {
        apply_color(b.r, b.g, b.b);
    }
}
