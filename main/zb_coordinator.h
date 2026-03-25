#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zb_coordinator_event_cb_t)(const char *event_name);

#define ZB_COORD_MAX_INTERVIEW_DUMP 32
#define ZB_COORD_MAX_IDENTITY_DUMP 16
#define ZB_COORD_EVENT_NAME_MAX 40
#define ZB_COORD_EVENT_STATUS_MAX 24

typedef struct {
    char name[ZB_COORD_EVENT_NAME_MAX];
    char status[ZB_COORD_EVENT_STATUS_MAX];
    bool has_device;
    bool has_status;
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t cluster_id;
    double ts_s;
} zb_coord_event_info_t;

typedef struct {
    bool used;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t phase;
    uint8_t retries;
    uint8_t ep_count;
    uint8_t ep_idx;
    bool node_desc_ok;
    bool active_ep_ok;
    bool simple_desc_ok;
    double last_seen_s;
    double last_interview_s;
} zb_coord_interview_dump_t;

typedef struct {
    bool used;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t attempts;
    bool got_manufacturer;
    bool got_model;
} zb_coord_identity_dump_t;

typedef struct {
    uint32_t version;
    bool has_network;
    uint8_t channel;
    uint16_t pan_id;
    uint16_t short_addr;
    uint8_t ext_pan_id[8];
    uint8_t network_key_seq;
} zb_network_runtime_t;

typedef struct {
    zb_network_runtime_t runtime;
    uint32_t interview_count;
    uint32_t identity_count;
    zb_coord_interview_dump_t interviews[ZB_COORD_MAX_INTERVIEW_DUMP];
    zb_coord_identity_dump_t identities[ZB_COORD_MAX_IDENTITY_DUMP];
} zb_coordinator_ram_snapshot_t;

esp_err_t zb_coordinator_init(zb_coordinator_event_cb_t event_cb);
esp_err_t zb_coordinator_set_permit_join(bool enable);
esp_err_t zb_coordinator_request_interview(uint16_t short_addr);
esp_err_t zb_coordinator_poll(void);
esp_err_t zb_coordinator_get_runtime_state(zb_network_runtime_t *out_state);
esp_err_t zb_coordinator_get_last_event_info(zb_coord_event_info_t *out_info);
esp_err_t zb_coordinator_local_reset(void);
void zb_coordinator_factory_reset(void);
void zb_coordinator_get_ram_snapshot(zb_coordinator_ram_snapshot_t *out);
/** Refresca last_seen del job de entrevista ante tráfico entrante del nodo (ZCL/ZDO vía callbacks o trazas RX). */
void zb_coordinator_note_inbound_device_traffic(uint16_t short_addr);

#ifdef __cplusplus
}
#endif
