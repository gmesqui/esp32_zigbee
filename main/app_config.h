#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_CONFIG_MDNS_HOSTNAME_LEN 32
#define APP_CONFIG_MDNS_INSTANCE_LEN 64
#define APP_CONFIG_NTP_SERVER_LEN 64
#define APP_CONFIG_TIMEZONE_LEN 48

#define APP_CONFIG_DEFAULT_MDNS_HOSTNAME "esp32-zigbee"
#define APP_CONFIG_DEFAULT_MDNS_INSTANCE "ESP32 Zigbee Coordinator"
#define APP_CONFIG_DEFAULT_PERMIT_JOIN_DURATION_S 180u
#define APP_CONFIG_DEFAULT_NTP_SERVER "time.google.com"
#define APP_CONFIG_DEFAULT_TIMEZONE "UTC0"
#define APP_CONFIG_DEFAULT_REPORT_ALWAYS_ON_MAX_S 300u
#define APP_CONFIG_DEFAULT_REPORT_SLEEPY_MAX_S 3600u
#define APP_CONFIG_DEFAULT_PRESENCE_PROBE_GRACE_S 20u
#define APP_CONFIG_DEFAULT_PRESENCE_OFFLINE_GRACE_S 40u

typedef struct {
    char mdns_hostname[APP_CONFIG_MDNS_HOSTNAME_LEN];
    char mdns_instance[APP_CONFIG_MDNS_INSTANCE_LEN];
    char ntp_server[APP_CONFIG_NTP_SERVER_LEN];
    char timezone[APP_CONFIG_TIMEZONE_LEN];
    uint16_t permit_join_duration_s;
    uint16_t report_always_on_max_s;
    uint16_t report_sleepy_max_s;
    uint16_t presence_probe_grace_s;
    uint16_t presence_offline_grace_s;
} app_config_t;

/** Load application configuration from NVS, falling back to defaults. */
void app_config_init(void);

/** Fill cfg with the built-in defaults. */
void app_config_defaults(app_config_t *cfg);

/** Copy the current configuration into cfg. */
void app_config_get(app_config_t *cfg);

/** Validate and persist a complete configuration snapshot. */
esp_err_t app_config_save(const app_config_t *cfg);

/** Hostname label accepted by mDNS: letters, numbers, hyphen, 1..31 chars. */
bool app_config_hostname_is_valid(const char *hostname);

/** Clamp user-provided permit-join duration into the supported range. */
uint16_t app_config_clamp_permit_join_duration(uint32_t duration_s);

uint16_t app_config_clamp_report_always_on_max(uint32_t seconds);
uint16_t app_config_clamp_report_sleepy_max(uint32_t seconds);
uint16_t app_config_clamp_presence_probe_grace(uint32_t seconds);
uint16_t app_config_clamp_presence_offline_grace(uint32_t seconds);
