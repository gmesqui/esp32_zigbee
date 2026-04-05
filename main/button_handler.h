#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Board pin — adjust if the BOOT button is on a different GPIO
// ESP32-C5-KITC-A V1.2: BOOT button = GPIO9
// ---------------------------------------------------------------------------
#define BOOT_BUTTON_GPIO    28

// Default project wiring uses GPIO28. If your board revision exposes the
// BOOT button on GPIO9 instead, change BOOT_BUTTON_GPIO before flashing.

// ---------------------------------------------------------------------------
// Initialise the BOOT button GPIO and the permit-join timer.
// Must be called after led_driver_init() and after the Zigbee stack is ready.
// ---------------------------------------------------------------------------
void button_handler_init(void);

// ---------------------------------------------------------------------------
// Called by zigbee_core when the stack is ready to accept permit-join calls.
// (The button ISR defers action until the stack is operational.)
// ---------------------------------------------------------------------------
void button_handler_set_stack_ready(bool ready);

// ---------------------------------------------------------------------------
// Returns true if permit-join is currently open.
// ---------------------------------------------------------------------------
bool button_handler_permit_join_active(void);

// ---------------------------------------------------------------------------
// Open or close permit-join for the specified duration.
// duration_s == 0 closes the network; any other value opens it and refreshes
// the expiry timer.
// ---------------------------------------------------------------------------
void button_handler_set_permit_join_duration(uint8_t duration_s);
