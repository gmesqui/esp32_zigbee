#include "timebase.h"

#include "esp_timer.h"

static int64_t s_t0_us = 0;

void timebase_init(void)
{
    s_t0_us = esp_timer_get_time();
}

double timebase_now_s(void)
{
    const int64_t now_us = esp_timer_get_time();
    return (double)(now_us - s_t0_us) / 1000000.0;
}
