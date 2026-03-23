#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zb_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_TABLE_MAX_DEVICES 64
#define DEVICE_TABLE_MAX_ENDPOINTS 8
#define DEVICE_TABLE_MAX_CLUSTERS 16
#define DEVICE_TABLE_MAX_STR 32

typedef struct {
    bool occupied;
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t endpoints[DEVICE_TABLE_MAX_ENDPOINTS];
    size_t endpoints_len;
    uint16_t clusters_in[DEVICE_TABLE_MAX_CLUSTERS];
    size_t clusters_in_len;
    uint16_t clusters_out[DEVICE_TABLE_MAX_CLUSTERS];
    size_t clusters_out_len;
    uint16_t device_id;
    int8_t rssi;
    uint8_t lqi;
    double last_seen_s;
    char manufacturer[DEVICE_TABLE_MAX_STR];
    char model[DEVICE_TABLE_MAX_STR];
    uint32_t state_flags;
} device_record_t;

void device_table_init(void);
void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi);
void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model);
void device_table_update_from_trace(const zb_trace_meta_t *meta);
void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model);
void device_table_update_node_desc(uint16_t short_addr, uint16_t manufacturer_code, uint8_t mac_capability_flags);
void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t device_id, const uint16_t *clusters_in,
                                     size_t clusters_in_len, const uint16_t *clusters_out, size_t clusters_out_len);
void device_table_dump_json(void);

#ifdef __cplusplus
}
#endif
