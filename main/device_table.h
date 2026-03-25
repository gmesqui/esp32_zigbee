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
    bool used;
    uint8_t endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t device_version;
    uint16_t input_clusters[DEVICE_TABLE_MAX_CLUSTERS];
    size_t input_clusters_len;
    uint16_t output_clusters[DEVICE_TABLE_MAX_CLUSTERS];
    size_t output_clusters_len;
    /** Ultimas lecturas ZCL del endpoint; no fuerzan persistencia. */
    bool has_temperature;
    bool has_humidity;
    bool has_on_off;
    bool has_occupancy;
    bool has_illuminance;
    bool has_pressure;
    bool has_ias_zone_status;
    bool has_power_battery_voltage;
    bool has_power_battery_pct;
    int16_t temperature_0_01_c;
    uint16_t humidity_0_01_pct;
    bool on_off;
    uint8_t occupancy_bitmap;
    uint16_t illuminance_measured_value;
    int16_t pressure_0_1_kpa;
    uint16_t ias_zone_status;
    uint16_t battery_mv;
    uint8_t battery_pct;
    double last_readings_update_s;
    double last_poll_read_s;
} device_endpoint_record_t;

typedef struct {
    bool occupied;
    bool in_network;
    bool seen_in_device_annce;
    bool authorized;
    uint8_t authorization_type;
    uint8_t authorization_status;
    uint64_t ieee;
    uint16_t short_addr;
    int8_t rssi;
    uint8_t lqi;
    double last_seen_s;
    char manufacturer[DEVICE_TABLE_MAX_STR];
    char model[DEVICE_TABLE_MAX_STR];
    uint16_t parent_short;
    uint8_t update_status;
    uint8_t tc_action;
    uint16_t node_desc_flags;
    uint8_t mac_capability_flags;
    uint16_t manufacturer_code;
    uint8_t max_buf_size;
    uint16_t max_incoming_transfer_size;
    uint16_t server_mask;
    uint16_t max_outgoing_transfer_size;
    uint8_t desc_capability_field;
    uint8_t interview_phase;
    uint8_t interview_retries;
    uint8_t report_cfg_retries;
    bool report_cfg_ok;
    bool silent;
    uint8_t silence_level; /* 0=activo, 1=silencio temporal, 2=ausencia prolongada */
    double last_interview_s;
    double last_report_cfg_s;
    device_norm_t norm_type;
    char norm_name[DEVICE_TABLE_MAX_TYPE_STR];
    size_t endpoint_count;
    device_endpoint_record_t endpoints[DEVICE_TABLE_MAX_ENDPOINTS];
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
    /** Informes ZCL Report Attributes recibidos con status OK (incluye valor repetido). */
    uint32_t report_attr_ok;
    /** Subconjunto de report_attr_ok sin cambio semantico en la tabla de lecturas. */
    uint32_t report_attr_unchanged;
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
void device_table_clear_cache_and_runtime(void);
void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi);
void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model);
void device_table_update_from_trace(const zb_trace_meta_t *meta);
/** Actualiza solo rssi/lqi y last_seen si el dispositivo existe (sin NVS ni note_inbound). */
void device_table_update_rf_metrics(uint16_t short_addr, int8_t rssi, uint8_t lqi);
void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model);
void device_table_update_node_desc(uint16_t short_addr, uint16_t node_desc_flags, uint8_t mac_capability_flags, uint16_t manufacturer_code,
                                   uint8_t max_buf_size, uint16_t max_incoming_transfer_size, uint16_t server_mask,
                                   uint16_t max_outgoing_transfer_size, uint8_t desc_capability_field);
void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t profile_id, uint16_t device_id, uint8_t device_version,
                                     const uint16_t *clusters_in, size_t clusters_in_len, const uint16_t *clusters_out,
                                     size_t clusters_out_len);
void device_table_update_device_update(uint64_t ieee, uint16_t short_addr, uint16_t parent_short, uint8_t status, uint8_t tc_action);
void device_table_update_authorization(uint64_t ieee, uint16_t short_addr, uint8_t authorization_type, uint8_t authorization_status);
void device_table_mark_leave(uint64_t ieee, uint16_t short_addr, bool rejoin);
void device_table_mark_interview(uint16_t short_addr, uint8_t phase, bool success, bool is_retry);
void device_table_mark_report_cfg(uint16_t short_addr, bool success, bool is_retry);
void device_table_mark_silent(uint16_t short_addr, bool silent);
void device_table_mark_absent_prolonged(uint16_t short_addr, bool absent);
void device_table_note_latency(bool is_zdo, double latency_ms);
void device_table_inc_counter(const char *counter_name);
void device_table_get_telemetry(device_table_telemetry_t *out);
void device_table_get_network_summary(device_table_network_summary_t *out);
size_t device_table_copy_devices(device_record_t *out, size_t max_out);
bool device_table_copy_device_at(size_t slot, device_record_t *out);
size_t device_table_get_known_short_addrs(uint16_t *out, size_t max_out);
bool device_table_has_short_addr(uint16_t short_addr);
/** @return true si el valor de lectura cambio (o es la primera vez); false si sin cambio o entrada invalida. */
bool device_table_note_reading_temperature(uint16_t short_addr, uint8_t ep, int16_t value_0_01_c);
bool device_table_note_reading_humidity(uint16_t short_addr, uint8_t ep, uint16_t value_0_01_pct);
bool device_table_note_reading_on_off(uint16_t short_addr, uint8_t ep, bool on);
bool device_table_note_reading_occupancy(uint16_t short_addr, uint8_t ep, uint8_t occupancy_bitmap);
bool device_table_note_reading_illuminance(uint16_t short_addr, uint8_t ep, uint16_t measured_value_raw);
bool device_table_note_reading_pressure(uint16_t short_addr, uint8_t ep, int16_t measured_value_0_1_kpa);
bool device_table_note_reading_ias_zone_status(uint16_t short_addr, uint8_t ep, uint16_t zone_status);
bool device_table_note_reading_battery_voltage(uint16_t short_addr, uint8_t ep, uint8_t voltage_100mv_units);
bool device_table_note_reading_battery_pct(uint16_t short_addr, uint8_t ep, uint8_t percentage_remaining_half_pct);
bool device_table_get_health_probe(uint16_t short_addr, uint8_t *ep_out, uint16_t *cluster_id_out, uint16_t *attr_id_out);
void device_table_note_poll_request(uint16_t short_addr, uint8_t ep);

typedef void (*device_table_zcl_read_req_fn_t)(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t attr_id);
/** Solicita lecturas ZCL para sensores conocidos (clusters_in + endpoints); el callback debe enviar Read Attribute. */
void device_table_request_sensor_poll_reads(device_table_zcl_read_req_fn_t fn);

/** Escribe la cache en NVS solo si hubo cambios desde el ultimo guardado (no usar para wear continuo). */
void device_table_persist_cache(void);
void device_table_dump_json(void);

#ifdef __cplusplus
}
#endif
