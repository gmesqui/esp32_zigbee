#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board_config.h"

// Board button settings are selected in board_config.h from CONFIG_IDF_TARGET.

void button_handler_init(void);

// Called by zigbee_core when the stack is ready to accept permit-join calls.
// The button ISR defers action until the stack is operational.
void button_handler_set_stack_ready(bool ready);

bool button_handler_permit_join_active(void);
uint32_t button_handler_permit_join_remaining_s(void);

// Open or close permit-join for the specified duration.
// duration_s == 0 closes the network; any other value opens it and refreshes
// the expiry timer.
void button_handler_set_permit_join_duration(uint8_t duration_s);
