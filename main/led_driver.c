#include "led_driver.h"
#include "utils.h"
#include <math.h>
#include "esp_timer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_attr.h"

// ---------------------------------------------------------------------------
// Internal state (only touched by led_task except the atomic flags)
// ---------------------------------------------------------------------------

static led_strip_handle_t s_strip = NULL;

// Atomic flags — written from any task/ISR, read only in led_task
static volatile bool     s_permit_join  = false;
static volatile uint32_t s_pulse_end_ms = 0;   // uptime_ms() when pulse expires

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline uint8_t clamp8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// ---------------------------------------------------------------------------
// LED task — runs at ~50 Hz
// ---------------------------------------------------------------------------

static void led_task(void *arg)
{
    (void)arg;
    // Period in ms
    const TickType_t period = pdMS_TO_TICKS(20);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        // ---- Base sine wave: green <-> blue, period 4 s ----
        float phase = (float)(now_ms % 4000u) * (2.0f * (float)M_PI / 4000.0f);
        float s = sinf(phase);
        int green = (int)(128.0f + 127.0f * s);
        int blue  = (int)(128.0f + 127.0f * (-s));   // antiphase
        int red   = 0;

        // ---- Permit-join overlay: red pulse 1 s period ----
        if (s_permit_join) {
            float jp = (float)(now_ms % 1000u) * (2.0f * (float)M_PI / 1000.0f);
            red = (int)(64.0f + 63.0f * sinf(jp));
        }

        // ---- Activity pulse: white flash that decays in 100 ms ----
        if (s_pulse_end_ms != 0) {
            int remaining = (int)(s_pulse_end_ms - now_ms);
            if (remaining > 0) {
                int boost = (remaining * 255) / 100;
                red   = clamp8(red   + boost);
                green = clamp8(green + boost);
                blue  = clamp8(blue  + boost);
            } else {
                s_pulse_end_ms = 0;
            }
        }

        led_strip_set_pixel(s_strip, 0,
                            clamp8(red), clamp8(green), clamp8(blue));
        led_strip_refresh(s_strip);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void led_driver_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num   = LED_STRIP_GPIO,
        .max_leds         = LED_STRIP_PIXELS,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,   // 10 MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    xTaskCreate(led_task, "led_task", 2048, NULL, 2, NULL);
    ZB_LOG("LED driver init OK (GPIO %d, WS2812)", LED_STRIP_GPIO);
}

void led_set_permit_join(bool active)
{
    s_permit_join = active;
}

void IRAM_ATTR led_trigger_activity_pulse(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_pulse_end_ms = now_ms + 100u;
}
