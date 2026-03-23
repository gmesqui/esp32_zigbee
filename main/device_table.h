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
#define DEVICE_TABLE_MAX_TYPE_STR 24

typedef enum {
    DEVICE_NORM_UNKNOWN = 0,
    DEVICE_NORM_SWITCH,
    DEVICE_NORM_TEMP_HUMIDITY,
    DEVICE_NORM_PRESENCE,
    DEVICE_NORM_TEMP,
} device_norm_t;

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
    uint16_t parent_short;
    uint8_t update_status;
    uint8_t interview_phase;
    uint8_t interview_retries;
    uint8_t report_cfg_retries;
    bool report_cfg_ok;
    bool silent;
    uint8_t silence_level; /* 0=activo, 1=silencio temporal, 2=ausencia prolongada */
    double last_interview_s;
    double last_report_cfg_s;
    double last_poll_read_s;
    uint16_t battery_mv;
    uint8_t battery_pct;
    device_norm_t norm_type;
    char norm_name[DEVICE_TABLE_MAX_TYPE_STR];
} device_record_t;

typedef struct {
    uint32_t interview_started;
    uint32_t interview_completed;
    uint32_t interview_retries;
    uint32_t interview_failed;
    uint32_t reinterviews;
    uint32_t read_req;
    uint32_t read_rsp_ok;
    uint32_t read_rsp_fail;
    uint32_t report_cfg_req;
    uint32_t report_cfg_rsp_ok;
    uint32_t report_cfg_rsp_fail;
    uint32_t device_announce;
    uint32_t device_update;
    uint32_t device_rejoin;
    uint32_t silent_nodes;
    double zdo_latency_avg_ms;
    double zcl_latency_avg_ms;
    uint32_t zdo_latency_samples;
    uint32_t zcl_latency_samples;
} device_table_telemetry_t;

typedef struct {
    uint32_t nodes_total;
    uint32_t nodes_silent_temp;
    uint32_t nodes_absent_prolonged;
    uint32_t nodes_switch;
    uint32_t nodes_temp_humidity;
    uint32_t nodes_presence;
    uint32_t nodes_unknown;
} device_table_network_summary_t;

typedef struct {
    uint16_t parent_short;
    uint32_t children;
} device_table_route_summary_t;

void device_table_init(void);
void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi);
void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model);
void device_table_update_from_trace(const zb_trace_meta_t *meta);
void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model);
void device_table_update_node_desc(uint16_t short_addr, uint16_t manufacturer_code, uint8_t mac_capability_flags);
void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t device_id, const uint16_t *clusters_in,
                                     size_t clusters_in_len, const uint16_t *clusters_out, size_t clusters_out_len);
void device_table_update_device_update(uint64_t ieee, uint16_t short_addr, uint16_t parent_short, uint8_t status);
void device_table_mark_interview(uint16_t short_addr, uint8_t phase, bool success, bool is_retry);
void device_table_mark_report_cfg(uint16_t short_addr, bool success, bool is_retry);
void device_table_mark_silent(uint16_t short_addr, bool silent);
void device_table_mark_absent_prolonged(uint16_t short_addr, bool absent);
void device_table_note_latency(bool is_zdo, double latency_ms);
void device_table_inc_counter(const char *counter_name);
void device_table_get_telemetry(device_table_telemetry_t *out);
void device_table_get_network_summary(device_table_network_summary_t *out);
size_t device_table_get_known_short_addrs(uint16_t *out, size_t max_out);
bool device_table_has_short_addr(uint16_t short_addr);
/** Escribe la cache en NVS solo si hubo cambios desde el ultimo guardado (no usar para wear continuo). */
void device_table_persist_cache(void);
void device_table_dump_json(void);

#ifdef __cplusplus
}
#endif
