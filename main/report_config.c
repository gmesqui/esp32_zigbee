#include "report_config.h"
#include "app_config.h"
#include "device_manager.h"
#include "device_interview.h"
#include "utils.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_zigbee_core.h"

#define RC_ZB_LOCK_WAIT_MS 1000u
#define RC_REPORT_CFG_RESPONSE_TIMEOUT_MS 12000u

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
    { 0x0001, 0x0020, ZCL_UINT8,  3600, 3600, 1  },  // Battery voltage (100 mV steps)
    { 0x0001, 0x0021, ZCL_UINT8,  3600, 3600, 2  },  // Battery %
    { 0x0500, 0x0002, ZCL_BMP16,  0,    3600, 0   },  // IAS Zone Status
    { 0x0B04, 0x050B, ZCL_INT16,  5,    3600, 10  },  // Active Power
};
#define REPORT_TABLE_COUNT  (sizeof(k_report_table) / sizeof(k_report_table[0]))

#define READ_REPORT_CFG_QUEUE_LEN           4

#define BIND_DST_ADDR_MODE_IEEE             0x03u

typedef struct {
    uint64_t ieee_addr;
    uint16_t nwk_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    bool in_use;
} read_report_cfg_req_t;

static read_report_cfg_req_t s_read_report_cfg_queue[READ_REPORT_CFG_QUEUE_LEN];

uint16_t rc_effective_max_interval(bool is_sleepy)
{
    app_config_t cfg;
    app_config_get(&cfg);
    return is_sleepy ? cfg.report_sleepy_max_s
                     : cfg.report_always_on_max_s;
}

uint32_t rc_presence_timeout_ms(bool is_sleepy)
{
    app_config_t cfg;
    app_config_get(&cfg);
    uint32_t max_interval_s = rc_effective_max_interval(is_sleepy);
    return (max_interval_s + cfg.presence_grace_s) * 1000u;
}

static void rc_configure_device_alarm(uint8_t dev_idx)
{
    device_record_t *dev = dm_get_by_index(dev_idx);
    if (!dev || !dev->in_use) {
        return;
    }
    rc_configure_device(dev);
}

static void rc_read_reporting_config_alarm(uint8_t slot)
{
    if (slot >= READ_REPORT_CFG_QUEUE_LEN) {
        return;
    }

    read_report_cfg_req_t req = s_read_report_cfg_queue[slot];
    s_read_report_cfg_queue[slot].in_use = false;
    if (!req.in_use) {
        return;
    }

    esp_zb_zcl_attribute_record_t record = {
        .report_direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID = req.attr_id,
    };
    esp_zb_zcl_read_report_config_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = COORD_ENDPOINT,
            .dst_addr_u.addr_short = req.nwk_addr,
            .dst_endpoint = req.endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = req.cluster_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 1,
        .manuf_specific = 0,
        .manuf_code = 0,
        .record_number = 1,
        .record_field = &record,
    };

    ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s read_report_cfg=[0x%04X]",
           req.nwk_addr, req.endpoint, utils_cluster_name(req.cluster_id),
           req.attr_id);
    esp_zb_zcl_read_report_config_cmd_req(&cmd);
}

static bool endpoint_has_input_cluster(const endpoint_record_t *ep, uint16_t cluster_id)
{
    if (!ep) return false;

    for (int i = 0; i < ep->in_cluster_count; i++) {
        if (ep->in_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static bool report_cfg_is_battery_attr(const report_cfg_entry_t *cfg)
{
    return cfg &&
           cfg->cluster_id == 0x0001 &&
           (cfg->attr_id == 0x0020 || cfg->attr_id == 0x0021);
}

static bool device_may_have_battery(const device_record_t *dev)
{
    if (!dev) {
        return true;
    }
    if (dev->power_source == 0x00) {
        return dev->is_sleepy;
    }
    return utils_power_source_may_have_battery(dev->power_source);
}

static bool report_cfg_applies_to_device(const device_record_t *dev,
                                         const report_cfg_entry_t *cfg)
{
    if (!report_cfg_is_battery_attr(cfg)) {
        return true;
    }

    return device_may_have_battery(dev);
}

static void report_cfg_records_reset(device_record_t *dev)
{
    if (!dev) {
        return;
    }
    dev->report_cfg_record_count = 0;
    dev->report_cfg_record_overflow = false;
    memset(dev->report_cfg_records, 0, sizeof(dev->report_cfg_records));
}

static void report_cfg_records_add(device_record_t *dev, uint8_t endpoint,
                                   const report_cfg_entry_t *cfg)
{
    if (!dev || !cfg) {
        return;
    }
    if (dev->report_cfg_record_count >= MAX_REPORT_CFG_TRACKED) {
        dev->report_cfg_record_overflow = true;
        return;
    }

    report_cfg_record_t *record =
        &dev->report_cfg_records[dev->report_cfg_record_count++];
    record->endpoint = endpoint;
    record->cluster_id = cfg->cluster_id;
    record->attr_id = cfg->attr_id;
    record->status = ESP_ZB_ZCL_STATUS_SUCCESS;
    record->result = REPORT_CFG_RESULT_PENDING;
}

static report_cfg_record_t *report_cfg_records_find(device_record_t *dev,
                                                    uint8_t endpoint,
                                                    uint16_t cluster_id,
                                                    uint16_t attr_id,
                                                    bool attr_known)
{
    if (!dev) {
        return NULL;
    }

    for (uint8_t i = 0; i < dev->report_cfg_record_count; i++) {
        report_cfg_record_t *record = &dev->report_cfg_records[i];
        if (record->result != REPORT_CFG_RESULT_PENDING) {
            continue;
        }
        if (record->endpoint != endpoint || record->cluster_id != cluster_id) {
            continue;
        }
        if (attr_known && record->attr_id != attr_id) {
            continue;
        }
        return record;
    }
    return NULL;
}

static void report_cfg_records_mark(device_record_t *dev, uint8_t endpoint,
                                    uint16_t cluster_id, uint16_t attr_id,
                                    bool attr_known, uint8_t status,
                                    report_cfg_result_t result)
{
    report_cfg_record_t *record =
        report_cfg_records_find(dev, endpoint, cluster_id, attr_id, attr_known);
    if (!record) {
        return;
    }
    record->status = status;
    record->result = (uint8_t)result;
}

static void report_cfg_records_add_failure(device_record_t *dev,
                                           uint8_t endpoint,
                                           uint16_t cluster_id,
                                           uint16_t attr_id,
                                           uint8_t status,
                                           report_cfg_result_t result)
{
    if (!dev) {
        return;
    }
    if (dev->report_cfg_record_count >= MAX_REPORT_CFG_TRACKED) {
        dev->report_cfg_record_overflow = true;
        return;
    }

    report_cfg_record_t *record =
        &dev->report_cfg_records[dev->report_cfg_record_count++];
    record->endpoint = endpoint;
    record->cluster_id = cluster_id;
    record->attr_id = attr_id;
    record->status = status;
    record->result = (uint8_t)result;
}

static bool endpoint_has_coord_binding(const endpoint_record_t *ep,
                                       uint16_t cluster_id,
                                       uint64_t coord_ieee)
{
    if (!ep || coord_ieee == 0) {
        return false;
    }

    for (int i = 0; i < ep->binding_count; i++) {
        const binding_record_t *binding = &ep->bindings[i];
        if (binding->cluster_id == cluster_id &&
            binding->dst_addr_mode == BIND_DST_ADDR_MODE_IEEE &&
            binding->dst_ieee_addr == coord_ieee &&
            binding->dst_endpoint == COORD_ENDPOINT) {
            return true;
        }
    }
    return false;
}

static void rc_on_bind_resp(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    uint32_t packed = (uint32_t)(uintptr_t)user_ctx;
    uint16_t nwk_addr = (uint16_t)(packed & 0xFFFFu);
    uint16_t cluster_id = (uint16_t)(packed >> 16);
    device_record_t *dev = dm_find_by_nwk(nwk_addr);
    const char *name = dev ? dm_display_name(dev) : "?";

    ZB_LOG("BIND_RSP %s nwk=0x%04X cluster=%s status=0x%02X",
           name, nwk_addr, utils_cluster_name(cluster_id),
           (unsigned)zdo_status);
    if (dev && zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        report_cfg_records_add_failure(dev, 0, cluster_id, 0xFFFF,
                                       (uint8_t)zdo_status,
                                       REPORT_CFG_RESULT_BIND_FAIL);
    }
}

static void bind_report_cluster_to_coord(device_record_t *dev,
                                         endpoint_record_t *ep,
                                         uint16_t cluster_id,
                                         uint64_t coord_ieee)
{
    if (!dev || !ep || endpoint_has_coord_binding(ep, cluster_id, coord_ieee)) {
        return;
    }

    esp_zb_zdo_bind_req_param_t req;
    memset(&req, 0, sizeof(req));
    memcpy(req.src_address, &dev->ieee_addr, sizeof(req.src_address));
    req.src_endp = ep->endpoint_id;
    req.cluster_id = cluster_id;
    req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
    memcpy(req.dst_address_u.addr_long, &coord_ieee,
           sizeof(req.dst_address_u.addr_long));
    req.dst_endp = COORD_ENDPOINT;
    req.req_dst_addr = dev->nwk_addr;

    ZB_LOG("BIND_REQ %s ep=%u cluster=%s -> coord_ep=%u",
           dm_display_name(dev), ep->endpoint_id,
           utils_cluster_name(cluster_id), COORD_ENDPOINT);
    esp_zb_zdo_device_bind_req(
        &req, rc_on_bind_resp,
        (void *)(uintptr_t)(((uint32_t)cluster_id << 16) | dev->nwk_addr));
}

size_t rc_get_configured_reportings_for_endpoint(const endpoint_record_t *ep,
                                                 bool is_sleepy,
                                                 rc_configured_reporting_t *out,
                                                 size_t out_len)
{
    size_t count = 0;

    if (!ep) return 0;

    for (size_t i = 0; i < REPORT_TABLE_COUNT; i++) {
        const report_cfg_entry_t *cfg = &k_report_table[i];
        if (!endpoint_has_input_cluster(ep, cfg->cluster_id)) {
            continue;
        }

        if (out && count < out_len) {
            out[count].cluster_id = cfg->cluster_id;
            out[count].attr_id = cfg->attr_id;
            out[count].minimum_report_interval = cfg->min_interval;
            out[count].maximum_report_interval = rc_effective_max_interval(is_sleepy);
            out[count].reportable_change = cfg->reportable_change;
        }
        count++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Send a Configure Reporting command for one cluster/attr on one endpoint.
//
// The SDK's reportable_change field is void* — it must point to a value of
// the correct ZCL type.  We hold the value in a local union that stays valid
// for the duration of the esp_zb_zcl_config_report_cmd_req() call (which
// copies internally before returning).
// ---------------------------------------------------------------------------

static void send_config_report(uint16_t nwk_addr, uint8_t endpoint,
                               bool is_sleepy, const report_cfg_entry_t *cfg)
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

    uint16_t max_interval = rc_effective_max_interval(is_sleepy);

    esp_zb_zcl_config_report_record_t record = {
        .direction         = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID       = cfg->attr_id,
        .attrType          = cfg->attr_type,
        .min_interval      = cfg->min_interval,
        .max_interval      = max_interval,
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
    ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s attr=0x%04X type=0x%02X "
           "cfg[min=%u max=%u change=%lu]",
           nwk_addr, endpoint, utils_cluster_name(cfg->cluster_id),
           cfg->attr_id, cfg->attr_type, cfg->min_interval, max_interval,
           (unsigned long)cfg->reportable_change);
    esp_zb_zcl_config_report_cmd_req(&cmd);

    ZB_LOG("REPORT_CFG -> 0x%04X ep=%u cluster=%s attr=0x%04X "
           "min=%u max=%u change=%lu",
           nwk_addr, endpoint, utils_cluster_name(cfg->cluster_id),
           cfg->attr_id, cfg->min_interval, max_interval,
           (unsigned long)cfg->reportable_change);
}

// ---------------------------------------------------------------------------
// Public: configure all relevant clusters on a device
// ---------------------------------------------------------------------------

size_t rc_configure_device(device_record_t *dev)
{
    if (!dev) return 0;

    if (!dm_has_complete_descriptors(dev)) {
        ZB_LOG("REPORT_CFG skip %s: incomplete descriptors, re-interview required",
               dm_display_name(dev));
        dev->report_cfg_expected = 0;
        dev->report_cfg_received = 0;
        dev->report_cfg_failed = 0;
        dev->report_cfg_in_progress = false;
        dev->report_cfg_started_ms = 0;
        if (dev->reporting_configured) {
            dev->reporting_configured = false;
            dev->dirty = true;
        }
        if (dev->state == DEV_STATE_CONFIGURED) {
            dev->state = DEV_STATE_NEW;
        }
        return 0;
    }

    ZB_LOG("REPORT_CFG start for %s (%s)",
           dm_display_name(dev), dev->is_sleepy ? "sleepy" : "always-on");

    int configured_count = 0;
    report_cfg_records_reset(dev);
    esp_zb_ieee_addr_t coord_addr;
    uint64_t coord_ieee = 0;
    esp_zb_get_long_address(coord_addr);
    memcpy(&coord_ieee, coord_addr, sizeof(coord_ieee));

    for (int e = 0; e < dev->endpoint_count; e++) {
        endpoint_record_t *ep = &dev->endpoints[e];
        uint16_t bound_clusters[MAX_CLUSTERS_PER_EP] = {0};
        uint8_t bound_cluster_count = 0;

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

            if (!report_cfg_applies_to_device(dev, cfg)) {
                ZB_LOG("REPORT_CFG skip %s ep=%u cluster=%s attr=0x%04X "
                       "power_source=%s",
                       dm_display_name(dev), ep->endpoint_id,
                       utils_cluster_name(cfg->cluster_id), cfg->attr_id,
                       utils_power_source_name(dev->power_source));
                continue;
            }

            bool bind_seen_this_pass = false;
            for (uint8_t b = 0; b < bound_cluster_count; b++) {
                if (bound_clusters[b] == cfg->cluster_id) {
                    bind_seen_this_pass = true;
                    break;
                }
            }
            if (!bind_seen_this_pass) {
                bind_report_cluster_to_coord(dev, ep, cfg->cluster_id, coord_ieee);
                if (bound_cluster_count < MAX_CLUSTERS_PER_EP) {
                    bound_clusters[bound_cluster_count++] = cfg->cluster_id;
                }
            }

            // For IAS Zone, write CIE address first
            if (cfg->cluster_id == 0x0500 && cfg->attr_id == 0x0002) {
                rc_write_ias_cie_address(dev->nwk_addr, ep->endpoint_id);
            }

            report_cfg_records_add(dev, ep->endpoint_id, cfg);
            send_config_report(dev->nwk_addr, ep->endpoint_id, dev->is_sleepy, cfg);
            configured_count++;
        }
    }

    ZB_LOG("REPORT_CFG sent %d configure-reporting commands to %s",
           configured_count, dm_display_name(dev));

    dev->report_cfg_expected = (uint16_t)configured_count;
    dev->report_cfg_received = 0;
    dev->report_cfg_failed = 0;
    dev->report_cfg_in_progress = (configured_count > 0);
    dev->report_cfg_started_ms = dev->report_cfg_in_progress ? utils_uptime_ms() : 0;
    bool reporting_configured =
        (configured_count == 0 && dm_has_complete_descriptors(dev));
    if (dev->reporting_configured != reporting_configured) {
        dev->reporting_configured = reporting_configured;
        dev->dirty = true;
    }
    if (reporting_configured && dev->state == DEV_STATE_INTERVIEWED) {
        dev->state = DEV_STATE_CONFIGURED;
    }

    return (size_t)configured_count;
}

static bool rc_report_cfg_timed_out(const device_record_t *dev, uint32_t now)
{
    return dev && dev->report_cfg_in_progress &&
           dev->report_cfg_started_ms != 0 &&
           (now - dev->report_cfg_started_ms) > RC_REPORT_CFG_RESPONSE_TIMEOUT_MS;
}

void rc_mark_reporting_timeout(device_record_t *dev)
{
    if (!dev || !dev->report_cfg_in_progress) {
        return;
    }

    uint16_t missing = 0;
    for (uint8_t i = 0; i < dev->report_cfg_record_count; i++) {
        report_cfg_record_t *record = &dev->report_cfg_records[i];
        if (record->result == REPORT_CFG_RESULT_PENDING) {
            record->result = REPORT_CFG_RESULT_MISSING;
            record->status = 0xFF;
            missing++;
        }
    }
    if (missing == 0 && dev->report_cfg_expected > dev->report_cfg_received) {
        missing = (uint16_t)(dev->report_cfg_expected - dev->report_cfg_received);
    }

    dev->report_cfg_in_progress = false;
    dev->report_cfg_started_ms = 0;
    if (missing > 0) {
        dev->report_cfg_failed = (uint16_t)(dev->report_cfg_failed + missing);
    }
    if (dev->reporting_configured) {
        dev->reporting_configured = false;
        dev->dirty = true;
    }
}

void rc_check_reporting_timeouts(void)
{
    uint32_t now = utils_uptime_ms();

    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        device_record_t *dev = dm_get_by_index(i);
        if (!dev || !rc_report_cfg_timed_out(dev, now)) {
            continue;
        }

        rc_mark_reporting_timeout(dev);
        if (dev->state == DEV_STATE_CONFIGURED) {
            dev->state = DEV_STATE_INTERVIEWED;
        }

        ZB_LOG("REPORT_CFG timeout for %s responses=%u/%u failed=%u",
               dm_display_name(dev), dev->report_cfg_received,
               dev->report_cfg_expected, dev->report_cfg_failed);
    }
}

bool rc_device_has_reporting_pending(const device_record_t *dev)
{
    if (!dev || !dev->in_use || !dev->is_sleepy) {
        return false;
    }
    if (dev->state < DEV_STATE_INTERVIEWED) {
        return false;
    }

    return !dev->reporting_configured || dev->report_cfg_in_progress;
}

bool rc_configure_pending_sleepy_now(device_record_t *dev, const char *reason)
{
    if (!rc_device_has_reporting_pending(dev)) {
        return false;
    }
    if (dev->report_cfg_in_progress) {
        ZB_LOG("REPORT_CFG sleepy window for %s reason=%s already in progress "
               "(responses=%u/%u failed=%u)",
               dm_display_name(dev), reason ? reason : "?",
               dev->report_cfg_received, dev->report_cfg_expected,
               dev->report_cfg_failed);
        return false;
    }

    ZB_LOG("REPORT_CFG sleepy window for %s reason=%s: configuring pending "
           "reporting",
           dm_display_name(dev), reason ? reason : "?");
    return rc_configure_device(dev) > 0;
}

void rc_configure_device_async(device_record_t *dev)
{
    int dev_idx;

    if (!dev) return;

    dev_idx = dm_index_of(dev);
    if (dev_idx < 0) return;

    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(RC_ZB_LOCK_WAIT_MS))) {
        ZB_LOG("REPORT_CFG lock timeout");
        return;
    }

    esp_zb_scheduler_alarm(rc_configure_device_alarm, (uint8_t)dev_idx, 0);
    esp_zb_lock_release();
}

bool rc_read_reporting_config_async(device_record_t *dev, uint8_t endpoint,
                                    uint16_t cluster_id, uint16_t attr_id)
{
    if (!dev || !dev->in_use || endpoint == 0) {
        return false;
    }

    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(RC_ZB_LOCK_WAIT_MS))) {
        ZB_LOG("READ_REPORT_CFG lock timeout");
        return false;
    }

    for (uint8_t i = 0; i < READ_REPORT_CFG_QUEUE_LEN; i++) {
        if (s_read_report_cfg_queue[i].in_use) {
            continue;
        }

        s_read_report_cfg_queue[i].ieee_addr = dev->ieee_addr;
        s_read_report_cfg_queue[i].nwk_addr = dev->nwk_addr;
        s_read_report_cfg_queue[i].endpoint = endpoint;
        s_read_report_cfg_queue[i].cluster_id = cluster_id;
        s_read_report_cfg_queue[i].attr_id = attr_id;
        s_read_report_cfg_queue[i].in_use = true;
        esp_zb_scheduler_alarm(rc_read_reporting_config_alarm, i, 0);
        esp_zb_lock_release();
        return true;
    }

    esp_zb_lock_release();
    return false;
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
    bool any_fail = (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS);

    const esp_zb_zcl_config_report_resp_variable_t *var = msg->variables;
    bool has_variables = false;
    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ZB_LOG("REPORT_CFG_RSP %s ep=%u cluster=%s CMD_FAIL status=0x%02X",
               name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
               (unsigned)msg->info.status);
    }
    while (var) {
        has_variables = true;
        if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            bool is_unsupported = (var->status == ESP_ZB_ZCL_STATUS_UNSUP_ATTRIB);
            ZB_LOG("REPORT_CFG_RSP %s ep=%u cluster=%s attr=0x%04X %s status=0x%02X",
                   name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, is_unsupported ? "UNSUPPORTED" : "FAIL",
                   (unsigned)var->status);
            if (!is_unsupported) {
                any_fail = true;
            }
            report_cfg_records_mark(dev, msg->info.src_endpoint,
                                    msg->info.cluster, var->attribute_id, true,
                                    var->status,
                                    is_unsupported ? REPORT_CFG_RESULT_UNSUPPORTED
                                                   : REPORT_CFG_RESULT_FAIL);
        } else {
            report_cfg_records_mark(dev, msg->info.src_endpoint,
                                    msg->info.cluster, var->attribute_id, true,
                                    var->status, REPORT_CFG_RESULT_OK);
        }
        var = var->next;
    }

    if (!any_fail) {
        ZB_LOG("REPORT_CFG_RSP %s ep=%u cluster=%s OK",
               name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster));
    }
    if (!has_variables) {
        report_cfg_records_mark(dev, msg->info.src_endpoint, msg->info.cluster,
                                0, false,
                                msg->info.status == ESP_ZB_ZCL_STATUS_SUCCESS
                                    ? ESP_ZB_ZCL_STATUS_SUCCESS
                                    : msg->info.status,
                                any_fail ? REPORT_CFG_RESULT_FAIL
                                         : REPORT_CFG_RESULT_OK);
    }

    if (dev && dev->report_cfg_in_progress) {
        if (dev->report_cfg_received < dev->report_cfg_expected) {
            dev->report_cfg_received++;
        }
        if (any_fail) {
            dev->report_cfg_failed++;
        }
        if (dev->report_cfg_received >= dev->report_cfg_expected) {
            dev->report_cfg_in_progress = false;
            dev->report_cfg_started_ms = 0;
            bool reporting_configured =
                (dev->report_cfg_failed == 0 && dm_has_complete_descriptors(dev));
            if (dev->reporting_configured != reporting_configured) {
                dev->reporting_configured = reporting_configured;
                dev->dirty = true;
            }
            if (reporting_configured && dev->state == DEV_STATE_INTERVIEWED) {
                dev->state = DEV_STATE_CONFIGURED;
            } else if (!reporting_configured && dev->state == DEV_STATE_CONFIGURED) {
                dev->state = DEV_STATE_INTERVIEWED;
            }
        }
    }

    di_on_reporting_config_response(dev);
}

void rc_on_read_report_cfg_resp(const esp_zb_zcl_cmd_read_report_config_resp_message_t *msg)
{
    if (!msg) {
        return;
    }

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    const char *name = dev ? dm_display_name(dev) : "?";

    const esp_zb_zcl_read_report_config_resp_variable_t *var = msg->variables;
    if (!var) {
        ZB_LOG("READ_REPORT_CFG_RSP %s ep=%u cluster=%s empty",
               name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster));
        return;
    }

    while (var) {
        if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            ZB_LOG("READ_REPORT_CFG_RSP %s ep=%u cluster=%s attr=0x%04X FAIL status=0x%02X",
                   name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, (unsigned)var->status);
        } else if (var->report_direction == ESP_ZB_ZCL_REPORT_DIRECTION_SEND) {
            ZB_LOG("READ_REPORT_CFG_RSP %s ep=%u cluster=%s attr=0x%04X dir=send type=0x%02X min=%u max=%u",
                   name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, var->client.attr_type,
                   var->client.min_interval, var->client.max_interval);
        } else {
            ZB_LOG("READ_REPORT_CFG_RSP %s ep=%u cluster=%s attr=0x%04X dir=recv timeout=%u",
                   name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, var->server.timeout);
        }
        var = var->next;
    }
}

void rc_on_write_attr_resp(const esp_zb_zcl_cmd_write_attr_resp_message_t *msg)
{
    if (!msg) {
        return;
    }

    uint16_t src_nwk = msg->info.src_address.u.short_addr;
    device_record_t *dev = dm_find_by_nwk(src_nwk);
    const char *name = dev ? dm_display_name(dev) : "?";
    bool any_fail = (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS);

    const esp_zb_zcl_write_attr_resp_variable_t *var = msg->variables;
    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ZB_LOG("WRITE_ATTR_RSP %s ep=%u cluster=%s CMD_FAIL status=0x%02X",
               name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
               (unsigned)msg->info.status);
    }
    while (var) {
        if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            any_fail = true;
            ZB_LOG("WRITE_ATTR_RSP %s ep=%u cluster=%s attr=0x%04X FAIL status=0x%02X",
                   name, msg->info.src_endpoint, utils_cluster_name(msg->info.cluster),
                   var->attribute_id, (unsigned)var->status);
            report_cfg_records_add_failure(dev, msg->info.src_endpoint,
                                           msg->info.cluster, var->attribute_id,
                                           (uint8_t)var->status,
                                           REPORT_CFG_RESULT_WRITE_FAIL);
        }
        var = var->next;
    }

    if (any_fail && !msg->variables) {
        report_cfg_records_add_failure(dev, msg->info.src_endpoint,
                                       msg->info.cluster, 0xFFFF,
                                       (uint8_t)msg->info.status,
                                       REPORT_CFG_RESULT_WRITE_FAIL);
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
