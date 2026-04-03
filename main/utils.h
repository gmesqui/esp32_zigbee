#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

/** Returns milliseconds elapsed since boot (monotonic). */
uint32_t utils_uptime_ms(void);

/** Returns seconds elapsed since boot as float. */
float utils_uptime_s(void);

// ---------------------------------------------------------------------------
// IEEE address helpers
// ---------------------------------------------------------------------------

/** Format a 64-bit IEEE address as "0xAABBCCDDEEFF0011" into buf. */
void utils_ieee_to_str(uint64_t ieee, char *buf, size_t len);

/** Parse "0xAABBCCDDEEFF0011" or "AABBCCDDEEFF0011" into ieee.
 *  Returns true on success. */
bool utils_str_to_ieee(const char *str, uint64_t *out_ieee);

// ---------------------------------------------------------------------------
// ZCL / Zigbee name helpers
// ---------------------------------------------------------------------------

/** Short human-readable name for a known cluster ID. Returns hex string for unknowns. */
const char *utils_cluster_name(uint16_t cluster_id);

/** Short name for a known HA profile device ID. */
const char *utils_device_type_name(uint16_t device_id);

/** Short name for a power_source value (Basic cluster 0x0007). */
const char *utils_power_source_name(uint8_t power_source);

/** Short name for a device_state enum value. */
const char *utils_device_state_name(int state);

// ---------------------------------------------------------------------------
// Logging macro — always uses [T+xxx.xxx] prefix on stdout
// ---------------------------------------------------------------------------

#define ZB_LOG(fmt, ...) \
    printf("[T+%07.3f] " fmt "\n", utils_uptime_s(), ##__VA_ARGS__)
