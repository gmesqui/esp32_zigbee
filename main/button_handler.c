#include "button_handler.h"
#include "led_driver.h"
#include "utils.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_zigbee_core.h"

#define DEBOUNCE_MS         200u
#define PERMIT_JOIN_SECS    180u

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static volatile bool     s_stack_ready    = false;
static volatile bool     s_permit_active  = false;
static volatile uint32_t s_last_press_ms  = 0;

static esp_timer_handle_t s_join_timer    = NULL;
static QueueHandle_t      s_btn_queue     = NULL;

// ---------------------------------------------------------------------------
// Permit-join open / close helpers
// ---------------------------------------------------------------------------

static void permit_join_open(void)
{
    if (s_permit_active) return;
    s_permit_active = true;
    led_set_permit_join(true);
    esp_zb_bdb_open_network(PERMIT_JOIN_SECS);
    esp_timer_start_once(s_join_timer, (uint64_t)PERMIT_JOIN_SECS * 1000000ULL);
    ZB_LOG("PERMIT_JOIN OPEN (%u s)", PERMIT_JOIN_SECS);
}

static void permit_join_close(void)
{
    if (!s_permit_active) return;
    s_permit_active = false;
    led_set_permit_join(false);
    esp_zb_bdb_close_network();
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
                permit_join_open();
            } else {
                permit_join_close();
            }
        } else {
            // Timer expired
            esp_zb_bdb_close_network();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void button_handler_init(void)
{
    s_btn_queue = xQueueCreate(4, sizeof(uint8_t));

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_isr_handler, NULL);

    // Create expiry timer
    esp_timer_create_args_t timer_args = {
        .callback  = join_timer_cb,
        .arg       = NULL,
        .name      = "join_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&timer_args, &s_join_timer);

    // Button processing task (small stack — just queued actions)
    xTaskCreate(btn_task, "btn_task", 2048, NULL, 3, NULL);

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
