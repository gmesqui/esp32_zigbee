#include "device_interview.h"
#include "zb_events.h"
#include "report_config.h"
#include "zcl_handler.h"
#include "nvs_cache.h"
#include "utils.h"
#include <string.h>

#define DI_ZB_LOCK_WAIT_MS 1000u
#include <stdio.h>
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"

// ---------------------------------------------------------------------------
// Interview state
// ---------------------------------------------------------------------------

typedef enum {
    ISTATE_IDLE,
    ISTATE_NODE_DESC,
    ISTATE_POWER_DESC,
    ISTATE_ACTIVE_EP,
    ISTATE_SIMPLE_DESC,   // repeated per endpoint
    ISTATE_READ_BASIC,
    ISTATE_READ_POWER_CFG,
    ISTATE_CONFIG_REPORT,
    ISTATE_WAIT_REPORT_CONFIG,
} istate_t;

typedef struct {
    uint8_t   dev_idx;          // index into g_devices
    uint64_t  ieee_addr;        // guards against slot reuse
    istate_t  state;
    uint8_t   ep_cursor;        // which endpoint we are currently querying
    uint8_t   retry;
    bool      active;
} interview_ctx_t;

static interview_ctx_t g_ictx;

// Simple FIFO queue of device indices awaiting interview
#define IQUEUE_SIZE  8
#define REPORT_CFG_RESPONSE_TIMEOUT_MS  8000u
static uint8_t g_iqueue[IQUEUE_SIZE];
static uint8_t g_iq_head = 0;
static uint8_t g_iq_tail = 0;
static uint8_t g_iq_count = 0;

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

static bool iq_push(uint8_t idx)
{
    if (g_iq_count >= IQUEUE_SIZE) return false;
    g_iqueue[g_iq_tail] = idx;
    g_iq_tail = (g_iq_tail + 1) % IQUEUE_SIZE;
    g_iq_count++;
    return true;
}

static bool iq_pop(uint8_t *idx)
{
    if (g_iq_count == 0) return false;
    *idx = g_iqueue[g_iq_head];
    g_iq_head = (g_iq_head + 1) % IQUEUE_SIZE;
    g_iq_count--;
    return true;
}

static void iq_remove(uint8_t idx)
{
    if (g_iq_count == 0) return;

    uint8_t tmp[IQUEUE_SIZE];
    uint8_t kept = 0;

    while (g_iq_count > 0) {
        uint8_t entry = 0;
        iq_pop(&entry);
        if (entry != idx) {
            tmp[kept++] = entry;
        }
    }

    g_iq_head = 0;
    g_iq_tail = 0;
    g_iq_count = 0;
    for (uint8_t i = 0; i < kept; i++) {
        iq_push(tmp[i]);
    }
}

// ---------------------------------------------------------------------------
// Device callback context helpers
// ---------------------------------------------------------------------------

static void *make_dev_ctx(uint8_t dev_idx)
{
    uintptr_t ctx = (uintptr_t)dev_idx;
    ctx |= ((uintptr_t)dm_slot_generation(dev_idx) << 8);
    return (void *)ctx;
}

static device_record_t *ctx_get_device(void *user_ctx, uint8_t *dev_idx_out)
{
    uintptr_t ctx = (uintptr_t)user_ctx;
    uint8_t dev_idx = (uint8_t)(ctx & 0xFFu);
    uint16_t generation = (uint16_t)((ctx >> 8) & 0xFFFFu);

    if (dev_idx_out) {
        *dev_idx_out = dev_idx;
    }

    return dm_get_by_index_generation(dev_idx, generation);
}

// ---------------------------------------------------------------------------
// Forward declarations of step functions (called via scheduler alarm)
// ---------------------------------------------------------------------------

static void interview_step(uint8_t dev_idx);    // dispatcher
static void alarm_start_step(uint8_t dev_idx);  // fired by scheduler
static void request_binding_table(device_record_t *dev, uint8_t start_index);
static void interview_fail(uint8_t dev_idx);
static void interview_done(uint8_t dev_idx);

static void format_attr_list(const uint16_t *attrs, uint8_t attr_count,
                             char *out, size_t out_len)
{
    size_t used = 0;

    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!attrs || attr_count == 0) return;

    for (uint8_t i = 0; i < attr_count; i++) {
        int n = snprintf(out + used, out_len - used, "%s0x%04X",
                         i ? "," : "", attrs[i]);
        if (n < 0 || (size_t)n >= out_len - used) {
            break;
        }
        used += (size_t)n;
    }
}

static void interview_finish_reporting_validation(uint8_t dev_idx, bool timed_out)
{
    device_record_t *dev = dm_get_by_index(dev_idx);
    if (!dev || !dev->in_use || dev->ieee_addr != g_ictx.ieee_addr) {
        g_ictx.ieee_addr = 0;
        g_ictx.state = ISTATE_IDLE;
        g_ictx.active = false;
        return;
    }

    if (timed_out && dev->report_cfg_in_progress) {
        dev->report_cfg_in_progress = false;
        if (dev->reporting_configured) {
            dev->reporting_configured = false;
            dev->dirty = true;
        }
    }

    if (!dev->report_cfg_in_progress &&
        dev->report_cfg_received == dev->report_cfg_expected &&
        dev->report_cfg_failed == 0 &&
        dev->reporting_configured) {
        ZB_LOG("INTERVIEW_FINAL %s SUCCESS reporting validated "
               "(responses=%u/%u failed=%u timeout=no)",
               dm_display_name(dev), dev->report_cfg_received,
               dev->report_cfg_expected, dev->report_cfg_failed);
        interview_done(dev_idx);
        return;
    }

    ZB_LOG("INTERVIEW_FINAL %s FAILED reporting validation "
           "(responses=%u/%u failed=%u timeout=%s)",
           dm_display_name(dev), dev->report_cfg_received,
           dev->report_cfg_expected, dev->report_cfg_failed,
           timed_out ? "yes" : "no");
    interview_fail(dev_idx);
}

#define BINDING_DST_ADDR_MODE_GROUP   0x01u
#define BINDING_DST_ADDR_MODE_IEEE    0x03u

// ---------------------------------------------------------------------------
// Helpers: send ZDO requests for each step
// ---------------------------------------------------------------------------

static void send_node_desc(uint16_t nwk_addr, uint8_t dev_idx)
{
    esp_zb_zdo_node_desc_req_param_t p = { .dst_nwk_addr = nwk_addr };
    ZB_LOG("TX RAW dst=0x%04X zdo=NODE_DESC_REQ", nwk_addr);
    esp_zb_zdo_node_desc_req(&p, di_on_node_desc_resp,
                              make_dev_ctx(dev_idx));
}

static void send_power_desc(uint16_t nwk_addr, uint8_t dev_idx)
{
    esp_zb_zdo_power_desc_req_param_t p = { .dst_nwk_addr = nwk_addr };
    ZB_LOG("TX RAW dst=0x%04X zdo=POWER_DESC_REQ", nwk_addr);
    esp_zb_zdo_power_desc_req(&p, di_on_power_desc_resp,
                               make_dev_ctx(dev_idx));
}

static void send_active_ep(uint16_t nwk_addr, uint8_t dev_idx)
{
    esp_zb_zdo_active_ep_req_param_t p = { .addr_of_interest = nwk_addr };
    ZB_LOG("TX RAW dst=0x%04X zdo=ACTIVE_EP_REQ", nwk_addr);
    esp_zb_zdo_active_ep_req(&p, di_on_active_ep_resp,
                              make_dev_ctx(dev_idx));
}

static void send_simple_desc(uint16_t nwk_addr, uint8_t ep_id, uint8_t dev_idx)
{
    esp_zb_zdo_simple_desc_req_param_t p = {
        .addr_of_interest = nwk_addr,
        .endpoint         = ep_id,
    };
    ZB_LOG("TX RAW dst=0x%04X ep=%u zdo=SIMPLE_DESC_REQ", nwk_addr, ep_id);
    esp_zb_zdo_simple_desc_req(&p, di_on_simple_desc_resp,
                                make_dev_ctx(dev_idx));
}

static void send_binding_table_req(uint16_t nwk_addr, uint8_t start_index,
                                   uint8_t dev_idx)
{
    esp_zb_zdo_mgmt_bind_param_t p = {
        .start_index = start_index,
        .dst_addr = nwk_addr,
    };
    ZB_LOG("TX RAW dst=0x%04X zdo=MGMT_BIND_REQ start_index=%u",
           nwk_addr, start_index);
    esp_zb_zdo_binding_table_req(&p, di_on_binding_table_resp,
                                 make_dev_ctx(dev_idx));
}

static void send_read_basic(uint16_t nwk_addr, uint8_t ep_id)
{
    static uint16_t attrs[] = { 0x0004, 0x0005, 0x0007 }; // mfr, model, power_src
    char attr_buf[48];
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint          = COORD_ENDPOINT,
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint          = ep_id,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID    = 0x0000,
        .attr_number  = 3,
        .attr_field   = attrs,
    };
    format_attr_list(attrs, 3, attr_buf, sizeof(attr_buf));
    ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s read_attrs=[%s]",
           nwk_addr, ep_id, utils_cluster_name(0x0000), attr_buf);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

static void send_read_power_cfg(uint16_t nwk_addr, uint8_t ep_id)
{
    static uint16_t attrs[] = { 0x0020, 0x0021 }; // battery_voltage, battery_pct
    char attr_buf[32];
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint          = COORD_ENDPOINT,
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint          = ep_id,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID    = 0x0001,
        .attr_number  = 2,
        .attr_field   = attrs,
    };
    format_attr_list(attrs, 2, attr_buf, sizeof(attr_buf));
    ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s read_attrs=[%s]",
           nwk_addr, ep_id, utils_cluster_name(0x0001), attr_buf);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

typedef struct {
    uint16_t cluster_id;
    uint8_t  attr_count;
    uint16_t attrs[3];
} startup_refresh_entry_t;

static const startup_refresh_entry_t k_startup_refresh_table[] = {
    { 0x0001, 2, { 0x0020, 0x0021 } },         // Battery voltage, battery %
    { 0x0006, 1, { 0x0000 } },                 // On/Off
    { 0x0008, 1, { 0x0000 } },                 // Level
    { 0x0300, 3, { 0x0000, 0x0001, 0x0007 } }, // Color control
    { 0x0402, 1, { 0x0000 } },                 // Temperature
    { 0x0405, 1, { 0x0000 } },                 // Humidity
    { 0x0403, 1, { 0x0000 } },                 // Pressure
    { 0x0400, 1, { 0x0000 } },                 // Illuminance
    { 0x0406, 1, { 0x0000 } },                 // Occupancy
    { 0x0500, 1, { 0x0002 } },                 // IAS Zone status
    { 0x0B04, 1, { 0x050B } },                 // Active power
};

typedef struct {
    uint8_t dev_idx;
    uint64_t ieee_addr;
    bool    active;
} startup_probe_ctx_t;

static startup_probe_ctx_t g_probe_ctx;

static void startup_probe_step(uint8_t dev_idx);
static void startup_probe_alarm(uint8_t dev_idx);

static void send_read_attrs(uint16_t nwk_addr, uint8_t ep_id,
                            uint16_t cluster_id, uint8_t attr_count,
                            const uint16_t *attrs)
{
    if (!attrs || attr_count == 0) return;

    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint          = COORD_ENDPOINT,
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint          = ep_id,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID    = cluster_id,
        .attr_number  = attr_count,
        .attr_field   = (uint16_t *)attrs,
    };
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

static bool endpoint_has_in_cluster(const endpoint_record_t *ep, uint16_t cluster_id)
{
    if (!ep) return false;
    for (int i = 0; i < ep->in_cluster_count; i++) {
        if (ep->in_clusters[i] == cluster_id) return true;
    }
    return false;
}

static bool device_may_have_battery(const device_record_t *dev)
{
    if (!dev) {
        return false;
    }
    if (dev->power_source == 0x00) {
        return dev->is_sleepy;
    }
    return utils_power_source_may_have_battery(dev->power_source);
}

static void probe_device_state(device_record_t *dev)
{
    if (!dev) return;

    uint8_t basic_ep = 0;
    if (dm_has_in_cluster(dev, 0x0000, &basic_ep)) {
        ZB_LOG("STARTUP_PROBE %s READ_BASIC ep=%u",
               dm_display_name(dev), basic_ep);
        send_read_basic(dev->nwk_addr, basic_ep);
    }

    for (int e = 0; e < dev->endpoint_count; e++) {
        endpoint_record_t *ep = &dev->endpoints[e];
        if (ep->endpoint_id == 0) continue;

        for (size_t i = 0; i < sizeof(k_startup_refresh_table) / sizeof(k_startup_refresh_table[0]); i++) {
            const startup_refresh_entry_t *entry = &k_startup_refresh_table[i];
            if (entry->cluster_id == 0x0001 && !device_may_have_battery(dev)) {
                continue;
            }
            if (!endpoint_has_in_cluster(ep, entry->cluster_id)) continue;

            ZB_LOG("STARTUP_PROBE %s READ cluster=%s ep=%u attrs=%u",
                   dm_display_name(dev), utils_cluster_name(entry->cluster_id),
                   ep->endpoint_id, entry->attr_count);
            send_read_attrs(dev->nwk_addr, ep->endpoint_id, entry->cluster_id,
                            entry->attr_count, entry->attrs);
        }
    }
}

static void startup_probe_schedule_next(uint8_t current_idx)
{
    for (uint8_t idx = current_idx + 1; idx < MAX_DEVICES; idx++) {
        device_record_t *dev = dm_get_by_index(idx);
        if (!dev || !dev->in_use) continue;
        if (dev->state < DEV_STATE_INTERVIEWED) continue;
        if (dev->is_sleepy) continue;

        g_probe_ctx.dev_idx = idx;
        g_probe_ctx.ieee_addr = dev->ieee_addr;
        g_probe_ctx.active = true;
        esp_zb_scheduler_alarm(startup_probe_alarm, idx, 1200);
        return;
    }

    g_probe_ctx.ieee_addr = 0;
    g_probe_ctx.active = false;
}

static void startup_probe_alarm(uint8_t dev_idx)
{
    startup_probe_step(dev_idx);
}

static void startup_probe_step(uint8_t dev_idx)
{
    if (!g_probe_ctx.active || g_probe_ctx.dev_idx != dev_idx) {
        return;
    }

    if (g_ictx.active) {
        esp_zb_scheduler_alarm(startup_probe_alarm, dev_idx, 1500);
        return;
    }

    device_record_t *dev = dm_get_by_index(dev_idx);
    if (!dev || !dev->in_use || dev->ieee_addr != g_probe_ctx.ieee_addr ||
        dev->state < DEV_STATE_INTERVIEWED || dev->is_sleepy) {
        startup_probe_schedule_next(dev_idx);
        return;
    }

    ZB_LOG("STARTUP_PROBE %s begin (%s)",
           dm_display_name(dev), dev->online ? "already-online" : "offline");
    probe_device_state(dev);
    request_binding_table(dev, 0);
    startup_probe_schedule_next(dev_idx);
}

// ---------------------------------------------------------------------------
// Advance interview to next step / finish
// ---------------------------------------------------------------------------

static void interview_fail(uint8_t dev_idx)
{
    device_record_t *dev = dm_get_by_index(dev_idx);
    if (dev) {
        dev->state = DEV_STATE_FAILED;
        dev->interview_attempts++;
        ZB_LOG("INTERVIEW %s FAILED (attempt %lu)",
               dm_display_name(dev),
               (unsigned long)dev->interview_attempts);

        zb_event_t evt = {
            .type             = ZB_EVT_INTERVIEW,
            .ieee             = dev->ieee_addr,
            .online           = dev->online,
            .interview_status = "failed",
        };
        strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
        zb_events_emit(&evt);
    }
    g_ictx.ieee_addr = 0;
    g_ictx.state = ISTATE_IDLE;
    g_ictx.active = false;

    // Try next device in queue
    uint8_t next;
    if (iq_pop(&next)) {
        esp_zb_scheduler_alarm(alarm_start_step, next, 500);
    }
}

static void interview_done(uint8_t dev_idx)
{
    device_record_t *dev = dm_get_by_index(dev_idx);
    if (dev) {
        dev->state = DEV_STATE_CONFIGURED;
        // Save immediately after interview
        nvs_cache_save_device(dev_idx);
        ZB_LOG("INTERVIEW %s COMPLETE (eps=%u mfr=%s model=%s %s)",
               dm_display_name(dev), dev->endpoint_count,
               dev->manufacturer[0] ? dev->manufacturer : "?",
               dev->model[0]        ? dev->model        : "?",
               dev->is_sleepy       ? "sleepy"          : "always-on");

        zb_event_t evt = {
            .type             = ZB_EVT_INTERVIEW,
            .ieee             = dev->ieee_addr,
            .online           = true,
            .interview_status = "successful",
        };
        strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
        zb_events_emit(&evt);

        // Also emit availability=online now that device is configured
        zb_event_t avail_evt = {
            .type   = ZB_EVT_AVAILABILITY,
            .ieee   = dev->ieee_addr,
            .online = true,
        };
        strncpy(avail_evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
        zb_events_emit(&avail_evt);

        request_binding_table(dev, 0);
    }
    g_ictx.ieee_addr = 0;
    g_ictx.state = ISTATE_IDLE;
    g_ictx.active = false;

    uint8_t next;
    if (iq_pop(&next)) {
        esp_zb_scheduler_alarm(alarm_start_step, next, 500);
    }
}

// ---------------------------------------------------------------------------
// Main step dispatcher — called via scheduler alarm
// ---------------------------------------------------------------------------

static void alarm_start_step(uint8_t dev_idx)
{
    interview_step(dev_idx);
}

static void interview_step(uint8_t dev_idx)
{
    if (!g_ictx.active || g_ictx.dev_idx != dev_idx) {
        return;
    }

    device_record_t *dev = dm_get_by_index(dev_idx);
    if (!dev || !dev->in_use || dev->ieee_addr != g_ictx.ieee_addr) {
        g_ictx.ieee_addr = 0;
        g_ictx.state = ISTATE_IDLE;
        g_ictx.active = false;
        return;
    }

    switch (g_ictx.state) {
        case ISTATE_NODE_DESC:
            ZB_LOG("INTERVIEW %s STEP_NODE_DESC", dm_display_name(dev));
            send_node_desc(dev->nwk_addr, dev_idx);
            break;

        case ISTATE_POWER_DESC:
            ZB_LOG("INTERVIEW %s STEP_POWER_DESC", dm_display_name(dev));
            send_power_desc(dev->nwk_addr, dev_idx);
            break;

        case ISTATE_ACTIVE_EP:
            ZB_LOG("INTERVIEW %s STEP_ACTIVE_EP", dm_display_name(dev));
            send_active_ep(dev->nwk_addr, dev_idx);
            break;

        case ISTATE_SIMPLE_DESC:
            if (g_ictx.ep_cursor < dev->endpoint_count) {
                uint8_t ep_id = dev->endpoints[g_ictx.ep_cursor].endpoint_id;
                ZB_LOG("INTERVIEW %s STEP_SIMPLE_DESC ep=%u [%u/%u]",
                       dm_display_name(dev), ep_id,
                       g_ictx.ep_cursor + 1, dev->endpoint_count);
                send_simple_desc(dev->nwk_addr, ep_id, dev_idx);
            } else {
                // All endpoints done, advance to READ_BASIC
                g_ictx.state = ISTATE_READ_BASIC;
                esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
            }
            break;

        case ISTATE_READ_BASIC:
        {
            // Use first endpoint that has Basic cluster
            uint8_t ep_id = 1;
            dm_has_in_cluster(dev, 0x0000, &ep_id);
            ZB_LOG("INTERVIEW %s STEP_READ_BASIC ep=%u", dm_display_name(dev), ep_id);
            send_read_basic(dev->nwk_addr, ep_id);
            // READ_BASIC result arrives via action handler → zcl_on_read_attr_resp.
            // Advance after delay — the response will have populated manufacturer/model.
            g_ictx.state = ISTATE_READ_POWER_CFG;
            esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 1500);
            break;
        }

        case ISTATE_READ_POWER_CFG:
        {
            uint8_t ep_id = 1;
            if (device_may_have_battery(dev) && dm_has_in_cluster(dev, 0x0001, &ep_id)) {
                ZB_LOG("INTERVIEW %s STEP_READ_POWER_CFG ep=%u",
                       dm_display_name(dev), ep_id);
                send_read_power_cfg(dev->nwk_addr, ep_id);
            } else if (dm_has_in_cluster(dev, 0x0001, NULL)) {
                ZB_LOG("INTERVIEW %s STEP_READ_POWER_CFG skipped power_source=%s",
                       dm_display_name(dev),
                       utils_power_source_name(dev->power_source));
            }
            // Advance after short delay regardless
            g_ictx.state = ISTATE_CONFIG_REPORT;
            esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 1000);
            break;
        }

        case ISTATE_CONFIG_REPORT:
            dev->state = DEV_STATE_INTERVIEWED;
            ZB_LOG("INTERVIEW %s STEP_CONFIG_REPORT", dm_display_name(dev));
            rc_configure_device(dev);
            g_ictx.state = ISTATE_WAIT_REPORT_CONFIG;
            if (dev->report_cfg_expected == 0) {
                interview_finish_reporting_validation(dev_idx, false);
            } else {
                esp_zb_scheduler_alarm(alarm_start_step, dev_idx,
                                       REPORT_CFG_RESPONSE_TIMEOUT_MS);
            }
            break;

        case ISTATE_WAIT_REPORT_CONFIG:
            interview_finish_reporting_validation(dev_idx,
                                                  dev->report_cfg_in_progress);
            break;

        default:
            interview_fail(dev_idx);
            break;
    }
}

// ---------------------------------------------------------------------------
// Public: start an interview
// ---------------------------------------------------------------------------

static void start_interview(uint8_t dev_idx)
{
    device_record_t *dev = dm_get_by_index(dev_idx);
    if (!dev) return;

    zcl_clear_unsupported_attrs(dev->ieee_addr);
    dev->state = DEV_STATE_INTERVIEWING;
    dev->interview_attempts++;

    g_ictx.dev_idx   = dev_idx;
    g_ictx.ieee_addr = dev->ieee_addr;
    g_ictx.state     = ISTATE_NODE_DESC;
    g_ictx.ep_cursor = 0;
    g_ictx.retry     = 0;
    g_ictx.active    = true;

    ZB_LOG("INTERVIEW %s START nwk=0x%04X (attempt %lu)",
           dm_display_name(dev), dev->nwk_addr,
           (unsigned long)dev->interview_attempts);

    zb_event_t evt = {
        .type             = ZB_EVT_INTERVIEW,
        .ieee             = dev->ieee_addr,
        .online           = dev->online,
        .interview_status = "started",
    };
    strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
    zb_events_emit(&evt);

    esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void di_init(void)
{
    memset(&g_ictx, 0, sizeof(g_ictx));
    memset(&g_probe_ctx, 0, sizeof(g_probe_ctx));
    g_iq_head = g_iq_tail = g_iq_count = 0;
}

void di_enqueue(device_record_t *dev)
{
    if (!dev) return;
    int idx = dm_index_of(dev);
    if (idx < 0) return;

    // Already in queue or being interviewed?
    if (g_ictx.active && g_ictx.dev_idx == (uint8_t)idx) return;
    for (int i = 0; i < (int)g_iq_count; i++) {
        if (g_iqueue[(g_iq_head + i) % IQUEUE_SIZE] == (uint8_t)idx) return;
    }

    if (!g_ictx.active) {
        start_interview((uint8_t)idx);
    } else {
        if (!iq_push((uint8_t)idx)) {
            ZB_LOG("INTERVIEW queue full, dropping %s", dm_display_name(dev));
        }
    }
}

bool di_enqueue_async(device_record_t *dev)
{
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(DI_ZB_LOCK_WAIT_MS))) {
        ZB_LOG("INTERVIEW lock timeout");
        return false;
    }

    di_enqueue(dev);
    esp_zb_lock_release();
    return true;
}

void di_on_reporting_config_response(device_record_t *dev)
{
    int idx;

    if (!dev || !g_ictx.active || g_ictx.state != ISTATE_WAIT_REPORT_CONFIG) {
        return;
    }

    idx = dm_index_of(dev);
    if (idx < 0) return;
    if ((uint8_t)idx != g_ictx.dev_idx) return;
    if (dev->ieee_addr != g_ictx.ieee_addr) return;
    if (dev->report_cfg_in_progress) return;

    esp_zb_scheduler_alarm(alarm_start_step, (uint8_t)idx, 0);
}

void di_trigger_ieee_resolve(uint16_t nwk_addr)
{
    esp_zb_zdo_ieee_addr_req_param_t p = {
        .dst_nwk_addr     = nwk_addr,
        .addr_of_interest = nwk_addr,
        .request_type     = 0,
        .start_index      = 0,
    };
    ZB_LOG("ZDO IEEE_ADDR_REQ nwk=0x%04X", nwk_addr);
    esp_zb_zdo_ieee_addr_req(&p, di_on_ieee_addr_resp,
                              (void *)(uintptr_t)nwk_addr);
}

void di_startup_probe_known_devices(void)
{
    if (g_probe_ctx.active) return;
    startup_probe_schedule_next((uint8_t)-1);
}

void di_forget_device(uint8_t dev_idx, uint64_t ieee)
{
    iq_remove(dev_idx);

    if (g_probe_ctx.active &&
        g_probe_ctx.dev_idx == dev_idx &&
        g_probe_ctx.ieee_addr == ieee) {
        g_probe_ctx.active = false;
        g_probe_ctx.ieee_addr = 0;
    }

    if (g_ictx.active &&
        g_ictx.dev_idx == dev_idx &&
        g_ictx.ieee_addr == ieee) {
        uint8_t next = 0;
        bool have_next = iq_pop(&next);

        g_ictx.active = false;
        g_ictx.state = ISTATE_IDLE;
        g_ictx.ieee_addr = 0;

        if (have_next) {
            esp_zb_scheduler_alarm(alarm_start_step, next, 100);
        }
    }
}

// ---------------------------------------------------------------------------
// ZDO response callbacks
// ---------------------------------------------------------------------------

void di_on_ieee_addr_resp(esp_zb_zdp_status_t zdo_status,
                           esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx)
{
    uint16_t nwk_addr = (uint16_t)(uintptr_t)user_ctx;

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !resp) {
        ZB_LOG("ZDO IEEE_ADDR_RSP nwk=0x%04X FAILED status=0x%02X",
               nwk_addr, (unsigned)zdo_status);
        return;
    }

    uint64_t ieee = 0;
    memcpy(&ieee, resp->ieee_addr, 8);

    char ibuf[20];
    utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
    ZB_LOG("ZDO IEEE_ADDR_RSP nwk=0x%04X ieee=%s", nwk_addr, ibuf);

    // Find by IEEE — may already exist (just changed short addr)
    device_record_t *dev = dm_find_by_ieee(ieee);
    if (dev) {
        dm_update_nwk(dev, nwk_addr);
        request_binding_table(dev, 0);
        // Replay any buffered ZCL attributes
        zcl_pending_attr_replay(ieee, nwk_addr);

        // If reporting not configured yet, re-enqueue for config
        if (!dev->reporting_configured && dev->state == DEV_STATE_CONFIGURED) {
            dev->state = DEV_STATE_INTERVIEWED;
            di_enqueue(dev);
        }
    } else {
        // Brand new device seen for the first time
        dev = dm_get_or_create(ieee, nwk_addr);
        if (dev) {
            zcl_pending_attr_replay(ieee, nwk_addr);
            di_enqueue(dev);
        }
    }
}

static void request_binding_table(device_record_t *dev, uint8_t start_index)
{
    int dev_idx;

    if (!dev || !dev->in_use) return;
    if (dev->nwk_addr == 0 || dev->nwk_addr == 0xFFFF) return;
    if (start_index == 0 && dev->binding_refresh_active) return;
    dev_idx = dm_index_of(dev);
    if (dev_idx < 0) return;
    if (start_index == 0) {
        dev->binding_refresh_active = true;
    }

    ZB_LOG("BINDING_TABLE_REQ %s start=%u nwk=0x%04X",
           dm_display_name(dev), start_index, dev->nwk_addr);
    send_binding_table_req(dev->nwk_addr, start_index,
                           (uint8_t)dev_idx);
}

void di_on_binding_table_resp(const esp_zb_zdo_binding_table_info_t *table_info,
                              void *user_ctx)
{
    uint8_t dev_idx = 0;
    device_record_t *dev = ctx_get_device(user_ctx, &dev_idx);

    if (!dev || !table_info || !dev->binding_refresh_active) {
        return;
    }

    if (table_info->status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        dev->binding_refresh_active = false;
        ZB_LOG("BINDING_TABLE_RSP %s FAILED status=0x%02X",
               dm_display_name(dev), table_info->status);
        return;
    }

    ZB_LOG("BINDING_TABLE_RSP %s index=%u count=%u total=%u",
           dm_display_name(dev), table_info->index,
           table_info->count, table_info->total);

    if (table_info->index == 0) {
        dm_clear_bindings(dev);
    }

    const esp_zb_zdo_binding_table_record_t *rec = table_info->record;
    while (rec) {
        binding_record_t binding = {
            .cluster_id = rec->cluster_id,
            .dst_addr_mode = rec->dst_addr_mode,
            .dst_group_addr = 0,
            .dst_ieee_addr = 0,
            .dst_endpoint = rec->dst_endp,
        };

        if (rec->dst_addr_mode == BINDING_DST_ADDR_MODE_GROUP) {
            binding.dst_group_addr = rec->dst_address.addr_short;
            binding.dst_endpoint = 0;
        } else if (rec->dst_addr_mode == BINDING_DST_ADDR_MODE_IEEE) {
            memcpy(&binding.dst_ieee_addr, rec->dst_address.addr_long, 8);
        }

        if (!dm_add_binding(dev, rec->src_endp, &binding)) {
            ZB_LOG("BINDING_TABLE_RSP %s drop src_ep=%u cluster=0x%04X",
                   dm_display_name(dev), rec->src_endp, rec->cluster_id);
        }
        rec = rec->next;
    }

    uint16_t next_index = (uint16_t)table_info->index + (uint16_t)table_info->count;
    if (next_index < table_info->total) {
        request_binding_table(dev, (uint8_t)next_index);
        return;
    }

    dev->binding_refresh_active = false;
    nvs_cache_save_device(dev_idx);
}

void di_on_node_desc_resp(esp_zb_zdp_status_t zdo_status, uint16_t addr,
                           esp_zb_af_node_desc_t *node_desc, void *user_ctx)
{
    uint8_t dev_idx = 0;
    device_record_t *dev = ctx_get_device(user_ctx, &dev_idx);

    if (!g_ictx.active || g_ictx.dev_idx != dev_idx ||
        !dev || dev->ieee_addr != g_ictx.ieee_addr) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !node_desc) {
        ZB_LOG("INTERVIEW %s NODE_DESC FAILED status=0x%02X",
               dev ? dm_display_name(dev) : "?", (unsigned)zdo_status);
        g_ictx.retry++;
        if (g_ictx.retry >= 3) {
            interview_fail(dev_idx);
        } else {
            esp_zb_scheduler_alarm(alarm_start_step, dev_idx,
                                   1000u << g_ictx.retry); // backoff
        }
        return;
    }

    g_ictx.retry = 0;
    if (dev) {
        bool next_is_sleepy = !(node_desc->mac_capability_flags & 0x08);
        if (dev->node_desc_flags != node_desc->node_desc_flags ||
            dev->mac_capability_flags != node_desc->mac_capability_flags ||
            dev->manufacturer_code != node_desc->manufacturer_code ||
            dev->is_sleepy != next_is_sleepy) {
            dev->node_desc_flags      = node_desc->node_desc_flags;
            dev->mac_capability_flags = node_desc->mac_capability_flags;
            dev->manufacturer_code    = node_desc->manufacturer_code;
            dev->is_sleepy            = next_is_sleepy;
            dev->dirty                = true;
        }

        ZB_LOG("INTERVIEW %s NODE_DESC OK addr=0x%04X mfr=0x%04X %s mac=0x%02X",
               dm_display_name(dev), addr, dev->manufacturer_code,
               dev->is_sleepy ? "SLEEPY" : "ALWAYS-ON",
               dev->mac_capability_flags);
    }

    g_ictx.state = ISTATE_POWER_DESC;
    esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
}

void di_on_power_desc_resp(esp_zb_zdo_power_desc_rsp_t *resp, void *user_ctx)
{
    uint8_t dev_idx = 0;
    device_record_t *dev = ctx_get_device(user_ctx, &dev_idx);

    if (!g_ictx.active || g_ictx.dev_idx != dev_idx ||
        !dev || dev->ieee_addr != g_ictx.ieee_addr) {
        return;
    }

    if (!resp || resp->status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ZB_LOG("INTERVIEW %s POWER_DESC FAILED (non-fatal, continuing)",
               dev ? dm_display_name(dev) : "?");
        // Power desc failure is non-fatal — continue with active EP
    } else if (dev) {
        // Store raw bitfield as uint16_t for persistence
        uint16_t flags = 0;
        memcpy(&flags, &resp->desc, sizeof(uint16_t) < sizeof(resp->desc)
                                     ? sizeof(uint16_t) : sizeof(resp->desc));
        if (dev->power_desc_flags != flags) {
            dev->power_desc_flags = flags;
            dev->dirty = true;
        }
        ZB_LOG("INTERVIEW %s POWER_DESC OK avail_src=0x%X cur_src=0x%X",
               dm_display_name(dev),
               resp->desc.available_power_sources,
               resp->desc.current_power_source);
    }

    g_ictx.state = ISTATE_ACTIVE_EP;
    esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
}

void di_on_active_ep_resp(esp_zb_zdp_status_t zdo_status, uint8_t ep_count,
                           uint8_t *ep_id_list, void *user_ctx)
{
    uint8_t dev_idx = 0;
    device_record_t *dev = ctx_get_device(user_ctx, &dev_idx);

    if (!g_ictx.active || g_ictx.dev_idx != dev_idx ||
        !dev || dev->ieee_addr != g_ictx.ieee_addr) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !ep_id_list) {
        ZB_LOG("INTERVIEW %s ACTIVE_EP FAILED status=0x%02X",
               dev ? dm_display_name(dev) : "?", (unsigned)zdo_status);
        g_ictx.retry++;
        if (g_ictx.retry >= 3) {
            interview_fail(dev_idx);
        } else {
            esp_zb_scheduler_alarm(alarm_start_step, dev_idx,
                                   1000u << g_ictx.retry);
        }
        return;
    }

    g_ictx.retry = 0;
    if (dev) {
        uint8_t cnt = ep_count;
        if (cnt > MAX_ENDPOINTS) cnt = MAX_ENDPOINTS;
        bool changed = (dev->endpoint_count != cnt);
        for (int i = 0; i < cnt; i++) {
            if (dev->endpoints[i].endpoint_id != ep_id_list[i]) {
                changed = true;
                break;
            }
        }
        dev->endpoint_count = cnt;
        for (int i = 0; i < cnt; i++) {
            dev->endpoints[i].endpoint_id = ep_id_list[i];
        }
        if (changed) {
            dev->dirty = true;
        }
        ZB_LOG("INTERVIEW %s ACTIVE_EP OK count=%u", dm_display_name(dev), cnt);
    }

    g_ictx.state     = ISTATE_SIMPLE_DESC;
    g_ictx.ep_cursor = 0;
    esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
}

void di_on_simple_desc_resp(esp_zb_zdp_status_t zdo_status,
                             esp_zb_af_simple_desc_1_1_t *simple_desc,
                             void *user_ctx)
{
    uint8_t dev_idx = 0;
    device_record_t *dev = ctx_get_device(user_ctx, &dev_idx);

    if (!g_ictx.active || g_ictx.dev_idx != dev_idx ||
        !dev || dev->ieee_addr != g_ictx.ieee_addr) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ZB_LOG("INTERVIEW %s SIMPLE_DESC ep_cursor=%u FAILED status=0x%02X",
               dev ? dm_display_name(dev) : "?", g_ictx.ep_cursor,
               (unsigned)zdo_status);
        // Skip failed endpoint, continue with next
        g_ictx.ep_cursor++;
        esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
        return;
    }

    if (dev) {
        uint8_t ep_idx = g_ictx.ep_cursor;
        if (ep_idx < MAX_ENDPOINTS) {
            endpoint_record_t *ep = &dev->endpoints[ep_idx];
            bool changed = false;
            if (ep->endpoint_id != simple_desc->endpoint ||
                ep->profile_id != simple_desc->app_profile_id ||
                ep->device_id != simple_desc->app_device_id ||
                ep->device_version != (uint8_t)simple_desc->app_device_version) {
                changed = true;
            }
            ep->endpoint_id    = simple_desc->endpoint;
            ep->profile_id     = simple_desc->app_profile_id;
            ep->device_id      = simple_desc->app_device_id;
            ep->device_version = (uint8_t)simple_desc->app_device_version;

            uint8_t in_count  = simple_desc->app_input_cluster_count;
            uint8_t out_count = simple_desc->app_output_cluster_count;
            if (in_count  > MAX_CLUSTERS_PER_EP) in_count  = MAX_CLUSTERS_PER_EP;
            if (out_count > MAX_CLUSTERS_PER_EP) out_count = MAX_CLUSTERS_PER_EP;

            if (ep->in_cluster_count != in_count ||
                ep->out_cluster_count != out_count) {
                changed = true;
            }
            if (simple_desc->app_cluster_list) {
                if (memcmp(ep->in_clusters, simple_desc->app_cluster_list,
                           in_count * sizeof(uint16_t)) != 0 ||
                    memcmp(ep->out_clusters,
                           simple_desc->app_cluster_list + in_count,
                           out_count * sizeof(uint16_t)) != 0) {
                    changed = true;
                }
            }

            ep->in_cluster_count  = in_count;
            ep->out_cluster_count = out_count;

            // cluster list: input clusters first, then output clusters
            if (simple_desc->app_cluster_list) {
                memcpy(ep->in_clusters, simple_desc->app_cluster_list,
                       in_count * sizeof(uint16_t));
                memcpy(ep->out_clusters,
                       simple_desc->app_cluster_list + in_count,
                       out_count * sizeof(uint16_t));
            }

            if (changed) {
                dev->dirty = true;
            }
            ZB_LOG("INTERVIEW %s SIMPLE_DESC ep=%u profile=0x%04X "
                   "dev_id=%s in=%u out=%u",
                   dm_display_name(dev), ep->endpoint_id, ep->profile_id,
                   utils_device_type_name(ep->device_id),
                   ep->in_cluster_count, ep->out_cluster_count);
        }
    }

    g_ictx.ep_cursor++;
    // Continue with next endpoint or advance state
    esp_zb_scheduler_alarm(alarm_start_step, dev_idx, 200);
}
