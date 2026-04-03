#include "report_config.h"
#include "device_manager.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------------------
// Table of clusters we want to configure reporting for.
// attr_type uses ZCL type codes.
// reportable_change: 0 for discrete types (bool, bitmap) → NULL pointer.
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t  attr_type;       // ZCL attribute type
    uint16_t min_interval;    // seconds
    uint16_t max_interval;    // seconds
    uint32_t reportable_change;  // 0 = discrete (no change threshold)
} report_cfg_entry_t;

// ZCL type codes matching the SDK constants
#define ZCL_BOOL    0x10
#define ZCL_UINT8   0x20
#define ZCL_UINT16  0x21
#define ZCL_INT16   0x29
#define ZCL_BMP8    0x18
#define ZCL_BMP16   0x19

static const report_cfg_entry_t k_report_table[] = {
    // cluster      attr    type      min   max   change
    { 0x0006, 0x0000, ZCL_BOOL,   0,    3600, 0   },  // On/Off
    { 0x0008, 0x0000, ZCL_UINT8,  1,    3600, 1   },  // Level
    { 0x0300, 0x0000, ZCL_UINT8,  1,    3600, 1   },  // Color Hue
    { 0x0300, 0x0001, ZCL_UINT8,  1,    3600, 1   },  // Color Saturation
    { 0x0300, 0x0007, ZCL_UINT16, 1,    3600, 10  },  // Color Temp Mireds
    { 0x0402, 0x0000, ZCL_INT16,  10,   3600, 10  },  // Temperature
    { 0x0405, 0x0000, ZCL_UINT16, 10,   3600, 50  },  // Humidity
    { 0x0403, 0x0000, ZCL_INT16,  10,   3600, 10  },  // Pressure
    { 0x0400, 0x0000, ZCL_UINT16, 10,   3600, 500 },  // Illuminance
    { 0x0406, 0x0000, ZCL_BMP8,   0,    3600, 0   },  // Occupancy
    { 0x0001, 0x0021, ZCL_UINT8,  3600, 43200, 2  },  // Battery %
    { 0x0500, 0x0002, ZCL_BMP16,  0,    3600, 0   },  // IAS Zone Status
    { 0x0B04, 0x050B, ZCL_INT16,  5,    3600, 10  },  // Active Power
};
#define REPORT_TABLE_COUNT  (sizeof(k_report_table) / sizeof(k_report_table[0]))

// ---------------------------------------------------------------------------
// Send a Configure Reporting command for one cluster/attr on one endpoint.
//
// The SDK's reportable_change field is void* — it must point to a value of
// the correct ZCL type.  We hold the value in a local union that stays valid
// for the duration of the esp_zb_zcl_config_report_cmd_req() call (which
// copies internally before returning).
// ---------------------------------------------------------------------------

static void send_config_report(uint16_t nwk_addr, uint8_t endpoint,
                                 const report_cfg_entry_t *cfg)
{
    // Storage for typed reportable_change value.
    // Must be in scope when esp_zb_zcl_config_report_cmd_req is called.
    union {
        uint8_t  u8;
        uint16_t u16;
        int16_t  s16;
    } rc_val;

    void *rc_ptr = NULL;
    if (cfg->reportable_change > 0) {
        switch (cfg->attr_type) {
            case ZCL_INT16:
                rc_val.s16 = (int16_t)cfg->reportable_change;
                rc_ptr = &rc_val.s16;
                break;
            case ZCL_UINT16:
                rc_val.u16 = (uint16_t)cfg->reportable_change;
                rc_ptr = &rc_val.u16;
                break;
            case ZCL_UINT8:
            case ZCL_BMP8:
                rc_val.u8 = (uint8_t)cfg->reportable_change;
                rc_ptr = &rc_val.u8;
                break;
            default:
                rc_ptr = NULL;
                break;
        }
    }
    // For discrete types (BOOL, BMP16, etc.) rc_ptr stays NULL — the SDK
    // ignores the reportable_change field for those types.

    esp_zb_zcl_config_report_record_t record = {
        .direction         = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID       = cfg->attr_id,
        .attrType          = cfg->attr_type,
        .min_interval      = cfg->min_interval,
        .max_interval      = cfg->max_interval,
        .reportable_change = rc_ptr,
    };

    esp_zb_zcl_config_report_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint          = COORD_ENDPOINT,
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint          = endpoint,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = cfg->cluster_id,
        .record_field  = &record,
        .record_number = 1,
    };
    esp_zb_zcl_config_report_cmd_req(&cmd);

    ZB_LOG("REPORT_CFG -> 0x%04X ep=%u cluster=%s attr=0x%04X "
           "min=%u max=%u change=%lu",
           nwk_addr, endpoint, utils_cluster_name(cfg->cluster_id),
           cfg->attr_id, cfg->min_interval, cfg->max_interval,
           (unsigned long)cfg->reportable_change);
}

// ---------------------------------------------------------------------------
// Public: configure all relevant clusters on a device
// ---------------------------------------------------------------------------

void rc_configure_device(device_record_t *dev)
{
    if (!dev) return;

    ZB_LOG("REPORT_CFG start for %s (%s)",
           dm_display_name(dev), dev->is_sleepy ? "sleepy" : "always-on");

    int configured_count = 0;

    for (int e = 0; e < dev->endpoint_count; e++) {
        endpoint_record_t *ep = &dev->endpoints[e];

        for (size_t t = 0; t < REPORT_TABLE_COUNT; t++) {
            const report_cfg_entry_t *cfg = &k_report_table[t];

            bool has_cluster = false;
            for (int c = 0; c < ep->in_cluster_count; c++) {
                if (ep->in_clusters[c] == cfg->cluster_id) {
                    has_cluster = true;
                    break;
                }
            }
            if (!has_cluster) continue;

            // For IAS Zone, write CIE address first
            if (cfg->cluster_id == 0x0500 && cfg->attr_id == 0x0002) {
                rc_write_ias_cie_address(dev->nwk_addr, ep->endpoint_id);
            }

            send_config_report(dev->nwk_addr, ep->endpoint_id, cfg);
            configured_count++;
        }
    }

    ZB_LOG("REPORT_CFG sent %d configure-reporting commands to %s",
           configured_count, dm_display_name(dev));

    if (configured_count > 0) {
        dev->reporting_configured = true;
        dev->dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Public: handle Configure Reporting Response
//
// The response uses esp_zb_zcl_cmd_info_t (field: info) for addressing.
// Individual attribute results are in msg->variables linked list.
// ---------------------------------------------------------------------------

void rc_on_config_resp(const esp_zb_zcl_cmd_config_report_resp_message_t *msg)
{
    if (!msg) return;

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    const char *name = dev ? dm_display_name(dev) : "?";

    // Iterate variable list for per-attribute status
    const esp_zb_zcl_config_report_resp_variable_t *var = msg->variables;
    bool any_fail = false;
    while (var) {
        if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            ZB_LOG("REPORT_CFG_RSP %s cluster=%s attr=0x%04X FAIL status=0x%02X",
                   name, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, (unsigned)var->status);
            any_fail = true;
        }
        var = var->next;
    }

    if (!any_fail) {
        ZB_LOG("REPORT_CFG_RSP %s cluster=%s OK",
               name, utils_cluster_name(msg->info.cluster));
    }
}

// ---------------------------------------------------------------------------
// Public: write coordinator IEEE to IAS_CIE_Address attribute (0x0010)
//
// Uses esp_zb_zcl_write_attr_cmd_t with attr_field / attr_number.
// The coordinator IEEE is stored in a static buffer so the pointer remains
// valid when esp_zb_zcl_write_attr_cmd_req() copies the data.
// ---------------------------------------------------------------------------

void rc_write_ias_cie_address(uint16_t nwk_addr, uint8_t endpoint)
{
    static esp_zb_ieee_addr_t s_coord_ieee;
    esp_zb_get_long_address(s_coord_ieee);

    // Build an esp_zb_zcl_attribute_t for attribute 0x0010 (IAS_CIE_Address)
    // Type 0xF0 = IEEE address (8 bytes)
    esp_zb_zcl_attribute_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.id         = 0x0010;
    attr.data.type  = 0xF0;          // ZCL type: IEEE address
    attr.data.value = s_coord_ieee;  // pointer to 8-byte IEEE addr

    esp_zb_zcl_write_attr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.zcl_basic_cmd.src_endpoint          = COORD_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = nwk_addr;
    cmd.zcl_basic_cmd.dst_endpoint          = endpoint;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID    = 0x0500;  // IAS Zone
    cmd.attr_number  = 1;
    cmd.attr_field   = &attr;

    esp_zb_zcl_write_attr_cmd_req(&cmd);

    ZB_LOG("IAS CIE_ADDR written to 0x%04X ep=%u", nwk_addr, endpoint);
}
