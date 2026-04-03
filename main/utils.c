#include "utils.h"
#include "device_manager.h"   // for device_state_t names
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

uint32_t utils_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

float utils_uptime_s(void)
{
    return (float)(esp_timer_get_time()) / 1000000.0f;
}

bool utils_wall_time_valid(void)
{
    time_t now = 0;
    struct tm utc_tm = {0};

    time(&now);
    gmtime_r(&now, &utc_tm);
    return utc_tm.tm_year >= (2024 - 1900);
}

void utils_format_log_prefix(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (!utils_wall_time_valid()) {
        snprintf(buf, len, "T+%07.3f", utils_uptime_s());
        return;
    }

    struct timeval tv = {0};
    struct tm utc_tm = {0};

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &utc_tm);

    snprintf(buf, len,
             "%04d-%02d-%02d %02d:%02d:%02d.%03ldZ",
             utc_tm.tm_year + 1900,
             utc_tm.tm_mon + 1,
             utc_tm.tm_mday,
             utc_tm.tm_hour,
             utc_tm.tm_min,
             utc_tm.tm_sec,
             tv.tv_usec / 1000L);
}

// ---------------------------------------------------------------------------
// IEEE helpers
// ---------------------------------------------------------------------------

void utils_ieee_to_str(uint64_t ieee, char *buf, size_t len)
{
    snprintf(buf, len, "0x%016llX", (unsigned long long)ieee);
}

bool utils_str_to_ieee(const char *str, uint64_t *out_ieee)
{
    if (!str || !out_ieee) return false;
    const char *p = str;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    char *end;
    uint64_t val = (uint64_t)strtoull(p, &end, 16);
    if (end == p) return false;
    *out_ieee = val;
    return true;
}

// ---------------------------------------------------------------------------
// Cluster names
// ---------------------------------------------------------------------------

typedef struct { uint16_t id; const char *name; } cluster_name_t;

static const cluster_name_t k_cluster_names[] = {
    { 0x0000, "BASIC"        },
    { 0x0001, "POWER_CFG"   },
    { 0x0003, "IDENTIFY"    },
    { 0x0004, "GROUPS"      },
    { 0x0005, "SCENES"      },
    { 0x0006, "ON_OFF"      },
    { 0x0008, "LEVEL"       },
    { 0x0020, "POLL_CTRL"   },
    { 0x0102, "WIN_COVER"   },
    { 0x0201, "THERMOSTAT"  },
    { 0x0300, "COLOR_CTRL"  },
    { 0x0400, "ILLUMINANCE" },
    { 0x0402, "TEMPERATURE" },
    { 0x0403, "PRESSURE"    },
    { 0x0405, "HUMIDITY"    },
    { 0x0406, "OCCUPANCY"   },
    { 0x0500, "IAS_ZONE"    },
    { 0x0501, "IAS_ACE"     },
    { 0x0702, "METERING"    },
    { 0x0B04, "ELEC_MEAS"  },
    { 0x0B05, "DIAGNOSTICS" },
    { 0x1000, "TOUCHLINK"   },
    { 0xEF00, "TUYA_PRIV"  },
};

const char *utils_cluster_name(uint16_t cluster_id)
{
    static char buf[12];
    for (size_t i = 0; i < sizeof(k_cluster_names)/sizeof(k_cluster_names[0]); i++) {
        if (k_cluster_names[i].id == cluster_id) return k_cluster_names[i].name;
    }
    snprintf(buf, sizeof(buf), "0x%04X", cluster_id);
    return buf;
}

// ---------------------------------------------------------------------------
// Device type names
// ---------------------------------------------------------------------------

const char *utils_device_type_name(uint16_t device_id)
{
    static char buf[12];
    switch (device_id) {
        case 0x0000: return "ON_OFF_SWITCH";
        case 0x0002: return "ON_OFF_OUTPUT";
        case 0x0051: return "SMART_PLUG";
        case 0x0100: return "ON_OFF_LIGHT";
        case 0x0101: return "DIMMABLE_LIGHT";
        case 0x0102: return "COLOR_DIMMABLE";
        case 0x0200: return "SHADE";
        case 0x0301: return "DOOR_LOCK";
        case 0x0302: return "TEMP_SENSOR";
        case 0x0303: return "OCCUPANCY_SENSOR";
        case 0x0106: return "LIGHT_SENSOR";
        case 0x0402: return "IAS_ZONE";
        case 0x0403: return "IAS_WARNING";
        default:
            snprintf(buf, sizeof(buf), "0x%04X", device_id);
            return buf;
    }
}

// ---------------------------------------------------------------------------
// Power source
// ---------------------------------------------------------------------------

const char *utils_power_source_name(uint8_t ps)
{
    switch (ps) {
        case 0x00: return "unknown";
        case 0x01: return "mains_single";
        case 0x02: return "mains_3phase";
        case 0x03: return "battery";
        case 0x04: return "dc";
        case 0x05: return "emergency_mains_batt";
        case 0x06: return "emergency_mains";
        default:   return "other";
    }
}

// ---------------------------------------------------------------------------
// Device state name
// ---------------------------------------------------------------------------

const char *utils_device_state_name(int state)
{
    switch (state) {
        case 0: return "new";
        case 1: return "interviewing";
        case 2: return "interviewed";
        case 3: return "configured";
        case 4: return "failed";
        default: return "unknown";
    }
}
