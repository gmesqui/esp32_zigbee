#pragma once
#include <stdbool.h>
#include "board_config.h"

// Board LED settings are selected in board_config.h from CONFIG_IDF_TARGET.

void led_driver_init(void);

// Runtime control (thread-safe, uses atomic flags)

/** Enable or disable the red permit-join overlay. */
void led_set_permit_join(bool active);

/** Trigger a brief white-flash pulse to indicate Zigbee radio activity.
 *  Safe to call from any context including ISR. */
void led_trigger_activity_pulse(void);
