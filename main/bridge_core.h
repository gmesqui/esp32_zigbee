#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_FRIENDLY_NAME_MAX 48
#define BRIDGE_MATTER_UNIQUE_ID_MAX 40

typedef enum {
    BRIDGE_EXPOSE_ON_OFF = 1UL << 0,
    BRIDGE_EXPOSE_TEMPERATURE = 1UL << 1,
    BRIDGE_EXPOSE_HUMIDITY = 1UL << 2,
    BRIDGE_EXPOSE_OCCUPANCY = 1UL << 3,
    BRIDGE_EXPOSE_ILLUMINANCE = 1UL << 4,
    BRIDGE_EXPOSE_PRESSURE = 1UL << 5,
    BRIDGE_EXPOSE_BATTERY = 1UL << 6,
} bridge_expose_mask_t;

typedef struct {
    uint8_t used;
    uint8_t hidden;
    uint8_t reserved0;
    uint8_t zigbee_norm_type;
    uint64_t zigbee_ieee;
    uint16_t matter_endpoint_id;
    uint16_t reserved1;
    uint32_t matter_device_id;
    uint32_t expose_mask;
    char friendly_name[BRIDGE_FRIENDLY_NAME_MAX];
    char matter_unique_id[BRIDGE_MATTER_UNIQUE_ID_MAX];
} bridge_device_binding_t;

typedef struct {
    uint8_t used;
    uint8_t supported;
    uint8_t reachable;
    uint8_t in_network;
    uint16_t zigbee_short_addr;
    uint8_t silence_level;
    uint8_t reserved0;
    double last_seen_s;
} bridge_device_state_t;

typedef struct {
    uint32_t version;
    uint32_t active_bindings;
    uint16_t next_matter_endpoint_id;
    uint8_t loaded_from_nvs;
    uint8_t dirty;
} bridge_registry_status_t;

void bridge_core_init(void);
void bridge_core_request_sync(void);
void bridge_core_poll(void);
void bridge_core_reset_registry(void);
void bridge_core_dump_registry_json(void);
void bridge_core_get_status(bridge_registry_status_t *out);
bool bridge_core_copy_binding_at(size_t slot, bridge_device_binding_t *binding_out, bridge_device_state_t *state_out);
bool bridge_core_copy_binding_by_ieee(uint64_t ieee, bridge_device_binding_t *binding_out, bridge_device_state_t *state_out);
bool bridge_core_set_matter_endpoint_id(uint64_t ieee, uint16_t matter_endpoint_id);
void bridge_core_format_friendly_name(const device_record_t *dev, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
