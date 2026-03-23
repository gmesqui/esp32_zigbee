#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZB_DIR_RX = 0,
    ZB_DIR_TX = 1,
} zb_trace_dir_t;

typedef struct {
    uint16_t src_short;
    uint16_t dst_short;
    uint16_t profile_id;
    uint16_t cluster_id;
    uint8_t src_ep;
    uint8_t dst_ep;
    uint8_t aps_counter;
    uint8_t nwk_seq;
    int8_t rssi;
    uint8_t lqi;
} zb_trace_meta_t;

uint32_t zb_trace_log_packet(zb_trace_dir_t dir, const zb_trace_meta_t *meta, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
