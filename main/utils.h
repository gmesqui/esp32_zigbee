#pragma once
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

/** Returns milliseconds elapsed since boot (monotonic). */
uint32_t utils_uptime_ms(void);

/** Returns seconds elapsed since boot as float. */
float utils_uptime_s(void);

/** Returns true when system wall-clock time is valid. */
bool utils_wall_time_valid(void);

/** Formats a log prefix with wall-clock time or fallback uptime. */
void utils_format_log_prefix(char *buf, size_t len);

/** Returns true when console logging should emit bytes. */
bool utils_console_log_enabled(void);

/** Write bytes to every active console sink. */
size_t utils_console_write(const char *data, size_t len);

/** Formatted console output routed to every active console sink. */
int utils_console_vprintf(const char *fmt, va_list ap);
int utils_console_printf(const char *fmt, ...);
int utils_console_putchar(int c);

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

/** True when Basic power_source is unknown or battery-backed. */
bool utils_power_source_may_have_battery(uint8_t power_source);

/** Short name for a device_state enum value. */
const char *utils_device_state_name(int state);

// ---------------------------------------------------------------------------
// Logging macros
// ---------------------------------------------------------------------------

#define ZB_LOG(fmt, ...)                                                     \
    do {                                                                     \
        if (utils_console_log_enabled()) {                                    \
            char _zb_log_prefix[32];                                         \
            utils_format_log_prefix(_zb_log_prefix, sizeof(_zb_log_prefix)); \
            utils_console_printf("[%s] " fmt "\n",                          \
                                 _zb_log_prefix, ##__VA_ARGS__);             \
        }                                                                    \
    } while (0)

#define ZB_PRINT(fmt, ...)                                                   \
    do {                                                                     \
        if (utils_console_log_enabled()) {                                    \
            char _zb_log_prefix[32];                                         \
            utils_format_log_prefix(_zb_log_prefix, sizeof(_zb_log_prefix)); \
            utils_console_printf("[%s] " fmt,                                \
                                 _zb_log_prefix, ##__VA_ARGS__);             \
        }                                                                    \
    } while (0)
