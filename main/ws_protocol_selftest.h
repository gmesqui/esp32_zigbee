#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t passed;
    uint16_t failed;
    uint16_t skipped;
} ws_protocol_selftest_result_t;

ws_protocol_selftest_result_t ws_protocol_selftest_run(bool verbose);

