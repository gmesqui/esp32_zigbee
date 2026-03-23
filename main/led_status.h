#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_BASE_BOOT = 0,
    LED_BASE_FORMING,
    LED_BASE_READY_CLOSED,
    LED_BASE_READY_OPEN,
    LED_BASE_ERROR,
} led_base_state_t;

typedef enum {
    LED_PULSE_NET = 0,
    LED_PULSE_EVENT,
    LED_PULSE_INTERVIEW,
} led_pulse_t;

void led_status_init(void);
void led_status_set_base(led_base_state_t state);
void led_status_pulse(led_pulse_t pulse);
void led_status_poll(void);

#ifdef __cplusplus
}
#endif
