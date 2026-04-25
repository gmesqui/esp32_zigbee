#include "button_handler.h"
#include "app_config.h"
#include "led_driver.h"
#include "utils.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_zigbee_core.h"
#include <inttypes.h>

#define DEBOUNCE_MS         200u
#define BUTTON_ZB_LOCK_WAIT_MS 1000u

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static volatile bool     s_stack_ready    = false;
static volatile bool     s_permit_active  = false;
static volatile uint32_t s_last_press_ms  = 0;
static volatile uint32_t s_permit_until_ms = 0;

static esp_timer_handle_t s_join_timer    = NULL;
static QueueHandle_t      s_btn_queue     = NULL;

static bool zb_lock_for_button_api(void)
{
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(BUTTON_ZB_LOCK_WAIT_MS))) {
        return true;
    }

    ZB_LOG("PERMIT_JOIN lock timeout");
    return false;
}

// ---------------------------------------------------------------------------
// Permit-join open / close helpers
// ---------------------------------------------------------------------------

static void permit_join_open(uint8_t duration_s)
{
    if (!zb_lock_for_button_api()) {
        return;
    }

    s_permit_active = true;
    s_permit_until_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) +
                        ((uint32_t)duration_s * 1000u);
    led_set_permit_join(true);
    esp_zb_bdb_open_network(duration_s);
    esp_zb_lock_release();
    esp_timer_stop(s_join_timer);
    esp_timer_start_once(s_join_timer, (uint64_t)duration_s * 1000000ULL);
    ZB_LOG("PERMIT_JOIN OPEN (%"PRIu8" s)", duration_s);
}

static void permit_join_close(void)
{
    if (!zb_lock_for_button_api()) {
        return;
    }

    s_permit_active = false;
    s_permit_until_ms = 0;
    led_set_permit_join(false);
    esp_zb_bdb_close_network();
    esp_zb_lock_release();
    esp_timer_stop(s_join_timer);
    ZB_LOG("PERMIT_JOIN CLOSED");
}

// ---------------------------------------------------------------------------
// Timer callback — fires when permit-join window expires
// ---------------------------------------------------------------------------

static void join_timer_cb(void *arg)
{
    (void)arg;
    s_permit_active = false;
    s_permit_until_ms = 0;
    led_set_permit_join(false);
    // Note: esp_zb_bdb_close_network() must be called from Zigbee task context.
    // We signal via the same queue the button uses.
    uint8_t ev = 0xFF; // timer-expired marker
    xQueueSendFromISR(s_btn_queue, &ev, NULL);
}

// ---------------------------------------------------------------------------
// GPIO ISR — minimal work, push event to queue
// ---------------------------------------------------------------------------

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if ((now_ms - s_last_press_ms) < DEBOUNCE_MS) return;
    s_last_press_ms = now_ms;

    uint8_t ev = 0x01; // button-press marker
    xQueueSendFromISR(s_btn_queue, &ev, NULL);
}

// ---------------------------------------------------------------------------
// Button processing task — runs outside Zigbee task so it can block
// ---------------------------------------------------------------------------

static void btn_task(void *arg)
{
    (void)arg;
    uint8_t ev;
    for (;;) {
        if (xQueueReceive(s_btn_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (!s_stack_ready) continue;

        if (ev == 0x01) {
            // Button press
            if (!s_permit_active) {
                app_config_t cfg;
                app_config_get(&cfg);
                permit_join_open((uint8_t)cfg.permit_join_duration_s);
            } else {
                permit_join_close();
            }
        } else {
            // Timer expired
            permit_join_close();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void button_handler_init(void)
{
    esp_err_t err;

    s_btn_queue = xQueueCreate(4, sizeof(uint8_t));
    if (!s_btn_queue) {
        ZB_LOG("ERROR button_handler: queue allocation failed");
        return;
    }

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ZB_LOG("ERROR button_handler: gpio_config(GPIO %d) failed: %s",
               BOOT_BUTTON_GPIO, esp_err_to_name(err));
        return;
    }

    // The shared GPIO ISR service is installed during system bring-up
    // before peripherals such as the W5500 and this button register handlers.
    err = gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_isr_handler, NULL);
    if (err != ESP_OK) {
        ZB_LOG("ERROR button_handler: gpio_isr_handler_add(GPIO %d) failed: %s",
               BOOT_BUTTON_GPIO, esp_err_to_name(err));
        return;
    }

    // Create expiry timer
    esp_timer_create_args_t timer_args = {
        .callback  = join_timer_cb,
        .arg       = NULL,
        .name      = "join_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    err = esp_timer_create(&timer_args, &s_join_timer);
    if (err != ESP_OK) {
        ZB_LOG("ERROR button_handler: esp_timer_create failed: %s",
               esp_err_to_name(err));
        return;
    }

    // Button processing task (small stack — just queued actions)
    if (xTaskCreate(btn_task, "btn_task", 2048, NULL, 3, NULL) != pdPASS) {
        ZB_LOG("ERROR button_handler: btn_task creation failed");
        return;
    }

    ZB_LOG("Button handler init OK (GPIO %d)", BOOT_BUTTON_GPIO);
}

void button_handler_set_stack_ready(bool ready)
{
    s_stack_ready = ready;
}

bool button_handler_permit_join_active(void)
{
    return s_permit_active;
}

uint32_t button_handler_permit_join_remaining_s(void)
{
    if (!s_permit_active || s_permit_until_ms == 0) {
        return 0;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if ((int32_t)(s_permit_until_ms - now_ms) <= 0) {
        return 0;
    }
    return (s_permit_until_ms - now_ms + 999u) / 1000u;
}

void button_handler_set_permit_join_duration(uint8_t duration_s)
{
    if (!s_stack_ready) {
        ZB_LOG("PERMIT_JOIN request ignored: stack not ready");
        return;
    }

    if (duration_s == 0) {
        permit_join_close();
    } else {
        permit_join_open(duration_s);
    }
}
