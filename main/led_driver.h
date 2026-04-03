#pragma once
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Board pin — adjust if the RGB LED is on a different GPIO
// ---------------------------------------------------------------------------
#define LED_STRIP_GPIO      27  // WS2812B data line
#define LED_STRIP_PIXELS    1   // single RGB LED

// ---------------------------------------------------------------------------
// Initialise the WS2812 driver and start the LED task.
// Must be called once from app_main before any other led_ call.
// ---------------------------------------------------------------------------
void led_driver_init(void);

// ---------------------------------------------------------------------------
// Runtime control (thread-safe, uses atomic flags)
// ---------------------------------------------------------------------------

/** Enable or disable the red permit-join overlay. */
void led_set_permit_join(bool active);

/** Trigger a brief white-flash pulse to indicate Zigbee radio activity.
 *  Safe to call from any context including ISR. */
void led_trigger_activity_pulse(void);
