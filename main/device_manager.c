#include "device_manager.h"
#include "report_config.h"
#include "zb_events.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"

// ---------------------------------------------------------------------------
// Global device table — static allocation
// ---------------------------------------------------------------------------

static device_record_t g_devices[MAX_DEVICES];
static uint8_t         g_count = 0;
static SemaphoreHandle_t g_mutex = NULL;

static void dm_clear_presence_probe(device_record_t *dev);

static void dm_emit_availability(const device_record_t *dev)
{
    if (!dev) return;

    zb_event_t evt = {
        .type   = ZB_EVT_AVAILABILITY,
        .ieee   = dev->ieee_addr,
        .online = dev->online,
    };
    strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
    evt.friendly_name[ZB_EVT_NAME_LEN - 1] = '\0';
    zb_events_emit(&evt);
}

static void dm_emit_device_updated(const device_record_t *dev)
{
    if (!dev) return;

    zb_event_t evt = {
        .type = ZB_EVT_DEVICE_UPDATED,
        .ieee = dev->ieee_addr,
        .online = dev->online,
        .lqi = dev->last_lqi,
        .has_lqi = dev->radio_metrics_valid,
    };
    strncpy(evt.friendly_name, dev->friendly_name, ZB_EVT_NAME_LEN - 1);
    evt.friendly_name[ZB_EVT_NAME_LEN - 1] = '\0';
    zb_events_emit(&evt);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void dm_init(void)
{
    memset(g_devices, 0, sizeof(g_devices));
    g_count = 0;
    g_mutex = xSemaphoreCreateMutex();
}

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

void dm_lock(void)   { xSemaphoreTake(g_mutex, portMAX_DELAY); }
void dm_unlock(void) { xSemaphoreGive(g_mutex); }

// ---------------------------------------------------------------------------
// Count
// ---------------------------------------------------------------------------

uint8_t dm_count(void) { return g_count; }

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

device_record_t *dm_find_by_ieee(uint64_t ieee)
{
    if (ieee == 0) return NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].in_use && g_devices[i].ieee_addr == ieee)
            return &g_devices[i];
    }
    return NULL;
}

device_record_t *dm_find_by_friendly_name(const char *name)
{
    if (!name || name[0] == '\0') return NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].in_use &&
            strcmp(g_devices[i].friendly_name, name) == 0) {
            return &g_devices[i];
        }
    }
    return NULL;
}

device_record_t *dm_find_by_nwk(uint16_t nwk_addr)
{
    if (nwk_addr == 0xFFFF) return NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].in_use && g_devices[i].nwk_addr == nwk_addr)
            return &g_devices[i];
    }
    return NULL;
}

device_record_t *dm_get_by_index(uint8_t idx)
{
    if (idx >= MAX_DEVICES) return NULL;
    if (!g_devices[idx].in_use) return NULL;
    return &g_devices[idx];
}

device_record_t *dm_get_by_index_generation(uint8_t idx, uint16_t generation)
{
    if (idx >= MAX_DEVICES) return NULL;
    if (!g_devices[idx].in_use) return NULL;
    if (g_devices[idx].slot_generation != generation) return NULL;
    return &g_devices[idx];
}

// ---------------------------------------------------------------------------
// Create / update
// ---------------------------------------------------------------------------

device_record_t *dm_get_or_create(uint64_t ieee, uint16_t nwk_addr)
{
    // Try to find existing entry
    device_record_t *dev = dm_find_by_ieee(ieee);
    if (dev) {
        // Update nwk_addr if it changed
        if (dev->nwk_addr != nwk_addr) {
            dev->nwk_addr = nwk_addr;
            dev->dirty = true;
        }
        return dev;
    }

    // Find a free slot
    if (g_count >= MAX_DEVICES) {
        ZB_LOG("ERROR dm_get_or_create: device table full");
        return NULL;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].in_use) {
            device_record_t *d = &g_devices[i];
            uint16_t next_generation = (uint16_t)(d->slot_generation + 1u);
            if (next_generation == 0) next_generation = 1;
            memset(d, 0, sizeof(*d));
            d->ieee_addr = ieee;
            d->nwk_addr  = nwk_addr;
            d->state     = DEV_STATE_NEW;
            d->slot_generation = next_generation;
            d->in_use    = true;
            d->dirty     = true;
            g_count++;

            char ibuf[20];
            utils_ieee_to_str(ieee, ibuf, sizeof(ibuf));
            ZB_LOG("DEVICE new slot %d ieee=%s nwk=0x%04X", i, ibuf, nwk_addr);
            return d;
        }
    }
    return NULL;
}

void dm_update_nwk(device_record_t *dev, uint16_t new_nwk_addr)
{
    if (!dev || dev->nwk_addr == new_nwk_addr) return;
    ZB_LOG("DEVICE %s nwk_addr 0x%04X -> 0x%04X",
           dm_display_name(dev), dev->nwk_addr, new_nwk_addr);
    dev->nwk_addr = new_nwk_addr;
    dev->dirty = true;
}

void dm_touch(device_record_t *dev, uint8_t lqi, int8_t rssi)
{
    if (!dev) return;
    dev->last_seen_ms = utils_uptime_ms();
    dm_clear_presence_probe(dev);
    if (!(lqi == 0 && rssi == 0)) {
        dev->last_lqi  = lqi;
        dev->last_rssi = rssi;
        dev->radio_metrics_valid = true;
    }

    if (!dev->online) {
        dev->online = true;
        ZB_LOG("DEVICE %s ONLINE nwk=0x%04X lqi=%u rssi=%d",
               dm_display_name(dev), dev->nwk_addr,
               dev->last_lqi, dev->last_rssi);
        dm_emit_availability(dev);
    }
}

bool dm_set_online(device_record_t *dev, bool online)
{
    if (!dev) return false;
    if (dev->online == online) return false;

    dev->online = online;
    if (!online) {
        dm_clear_presence_probe(dev);
    }
    ZB_LOG("DEVICE %s %s", dm_display_name(dev), online ? "ONLINE" : "OFFLINE");
    dm_emit_availability(dev);
    return true;
}

void dm_set_friendly_name(device_record_t *dev, const char *name)
{
    if (!dev || !name) return;

    char next_name[FRIENDLY_NAME_LEN];
    strncpy(next_name, name, sizeof(next_name) - 1);
    next_name[sizeof(next_name) - 1] = '\0';
    if (strcmp(dev->friendly_name, next_name) == 0) {
        return;
    }

    strncpy(dev->friendly_name, next_name, FRIENDLY_NAME_LEN - 1);
    dev->friendly_name[FRIENDLY_NAME_LEN - 1] = '\0';
    dev->dirty = true;
    ZB_LOG("DEVICE %s friendly_name set to \"%s\"",
           dm_display_name(dev), dev->friendly_name);
    dm_emit_device_updated(dev);
}

int dm_index_of(const device_record_t *dev)
{
    if (!dev) return -1;

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (&g_devices[i] == dev) {
            return i;
        }
    }

    return -1;
}

int dm_remove(device_record_t *dev)
{
    int idx = dm_index_of(dev);
    if (idx < 0) return -1;

    device_record_t *slot = &g_devices[idx];
    if (!slot->in_use) return -1;

    char ibuf[20];
    utils_ieee_to_str(slot->ieee_addr, ibuf, sizeof(ibuf));

    uint16_t next_generation = (uint16_t)(slot->slot_generation + 1u);
    if (next_generation == 0) next_generation = 1;

    memset(slot, 0, sizeof(*slot));
    slot->slot_generation = next_generation;
    if (g_count > 0) {
        g_count--;
    }

    ZB_LOG("DEVICE removed slot=%d ieee=%s", idx, ibuf);
    return idx;
}

uint16_t dm_slot_generation(uint8_t idx)
{
    if (idx >= MAX_DEVICES) return 0;
    return g_devices[idx].slot_generation;
}

// ---------------------------------------------------------------------------
// Presence check
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
} presence_probe_attr_t;

static const presence_probe_attr_t k_presence_probe_attrs[] = {
    { 0x0000, 0x0000 }, // Basic: ZCLVersion
    { 0x0006, 0x0000 }, // On/Off
    { 0x0008, 0x0000 }, // CurrentLevel
    { 0x0402, 0x0000 }, // Temperature
    { 0x0405, 0x0000 }, // Humidity
    { 0x0403, 0x0000 }, // Pressure
    { 0x0400, 0x0000 }, // Illuminance
    { 0x0406, 0x0000 }, // Occupancy
    { 0x0500, 0x0002 }, // IAS ZoneStatus
    { 0x0B04, 0x050B }, // ElectricalMeasurement ActivePower
    { 0x0001, 0x0021 }, // BatteryPercentageRemaining
};

static bool dm_select_presence_probe_attr(const device_record_t *dev,
                                          uint8_t *endpoint,
                                          uint16_t *cluster_id,
                                          uint16_t *attr_id)
{
    if (!dev || !endpoint || !cluster_id || !attr_id) {
        return false;
    }

    for (size_t i = 0; i < sizeof(k_presence_probe_attrs) / sizeof(k_presence_probe_attrs[0]); i++) {
        uint8_t ep = 0;
        if (dm_has_in_cluster(dev, k_presence_probe_attrs[i].cluster_id, &ep)) {
            *endpoint = ep;
            *cluster_id = k_presence_probe_attrs[i].cluster_id;
            *attr_id = k_presence_probe_attrs[i].attr_id;
            return true;
        }
    }

    if (dev->endpoint_count > 0 && dev->endpoints[0].endpoint_id != 0) {
        const endpoint_record_t *ep = &dev->endpoints[0];
        *endpoint = ep->endpoint_id;
        *cluster_id = 0x0000;
        *attr_id = 0x0000;
        return true;
    }

    return false;
}

bool dm_request_presence_probe(device_record_t *dev, const char *reason)
{
    if (!dev || !dev->in_use || dev->is_sleepy || !dev->online) {
        return false;
    }

    uint32_t now = utils_uptime_ms();
    if (dev->presence_probe_sent_ms != 0 &&
        dev->presence_probe_last_seen_ms == dev->last_seen_ms) {
        return true;
    }

    uint8_t endpoint = 0;
    uint16_t cluster_id = 0;
    uint16_t attr_id = 0;
    if (!dm_select_presence_probe_attr(dev, &endpoint, &cluster_id, &attr_id)) {
        ZB_LOG("PRESENCE probe skip %s reason=%s: no readable endpoint",
               dm_display_name(dev), reason ? reason : "?");
        return false;
    }

    uint16_t attr = attr_id;
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = COORD_ENDPOINT,
            .dst_addr_u.addr_short = dev->nwk_addr,
            .dst_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attr_number = 1,
        .attr_field = &attr,
    };

    dev->presence_probe_sent_ms = now;
    dev->presence_probe_last_seen_ms = dev->last_seen_ms;
    dev->presence_probe_endpoint = endpoint;
    dev->presence_probe_cluster_id = cluster_id;
    dev->presence_probe_attr_id = attr_id;

    ZB_LOG("PRESENCE probe %s reason=%s age=%lu s dst=0x%04X ep=%u cluster=%s attr=0x%04X",
           dm_display_name(dev), reason ? reason : "?",
           (unsigned long)((now - dev->last_seen_ms) / 1000u),
           dev->nwk_addr, endpoint, utils_cluster_name(cluster_id), attr_id);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    return true;
}

void dm_check_presence(void)
{
    uint32_t now = utils_uptime_ms();
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = &g_devices[i];
        if (!d->in_use || !d->online) continue;
        if (d->last_seen_ms == 0) continue;

        uint32_t offline_threshold = rc_presence_offline_timeout_ms(d->is_sleepy);
        uint32_t age_ms = now - d->last_seen_ms;

        if (d->is_sleepy) {
            if (age_ms <= offline_threshold) {
                continue;
            }
        } else if (age_ms > offline_threshold &&
                   d->presence_probe_sent_ms != 0 &&
                   d->presence_probe_last_seen_ms == d->last_seen_ms) {
            /* fall through to offline certification */
        } else {
            if (age_ms > rc_presence_probe_timeout_ms(false)) {
                if (!dm_request_presence_probe(d, "timeout") &&
                    age_ms > offline_threshold) {
                    /* No safe read route exists; certify by timeout. */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (age_ms > offline_threshold) {
            uint32_t secs = age_ms / 1000u;
            if (dm_set_online(d, false)) {
                ZB_LOG("DEVICE %s OFFLINE (no contact %lu s)", dm_display_name(d),
                       (unsigned long)secs);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Display name
// ---------------------------------------------------------------------------

const char *dm_display_name(const device_record_t *dev)
{
    if (!dev) return "(null)";
    if (dev->friendly_name[0] != '\0') return dev->friendly_name;
    static char buf[20];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)dev->ieee_addr);
    return buf;
}

// ---------------------------------------------------------------------------
// Cluster helpers
// ---------------------------------------------------------------------------

bool dm_has_in_cluster(const device_record_t *dev, uint16_t cluster_id,
                        uint8_t *ep_out)
{
    if (!dev) return false;
    for (int e = 0; e < dev->endpoint_count; e++) {
        const endpoint_record_t *ep = &dev->endpoints[e];
        for (int c = 0; c < ep->in_cluster_count; c++) {
            if (ep->in_clusters[c] == cluster_id) {
                if (ep_out) *ep_out = ep->endpoint_id;
                return true;
            }
        }
    }
    return false;
}

static void dm_clear_presence_probe(device_record_t *dev)
{
    if (!dev) {
        return;
    }
    dev->presence_probe_sent_ms = 0;
    dev->presence_probe_last_seen_ms = 0;
    dev->presence_probe_endpoint = 0;
    dev->presence_probe_cluster_id = 0;
    dev->presence_probe_attr_id = 0;
}

bool dm_has_complete_descriptors(const device_record_t *dev)
{
    if (!dev || dev->endpoint_count == 0 || dev->endpoint_count > MAX_ENDPOINTS) {
        return false;
    }

    for (uint8_t e = 0; e < dev->endpoint_count; e++) {
        const endpoint_record_t *ep = &dev->endpoints[e];
        if (ep->endpoint_id == 0 || ep->profile_id == 0) {
            return false;
        }
        if (ep->in_cluster_count > MAX_CLUSTERS_PER_EP ||
            ep->out_cluster_count > MAX_CLUSTERS_PER_EP) {
            return false;
        }
        if ((ep->in_cluster_count + ep->out_cluster_count) == 0) {
            return false;
        }
    }

    return true;
}

void dm_clear_bindings(device_record_t *dev)
{
    if (!dev) return;

    bool changed = false;
    for (int e = 0; e < dev->endpoint_count && e < MAX_ENDPOINTS; e++) {
        if (dev->endpoints[e].binding_count > 0) {
            changed = true;
        }
        dev->endpoints[e].binding_count = 0;
        memset(dev->endpoints[e].bindings, 0, sizeof(dev->endpoints[e].bindings));
    }
    if (changed) {
        dev->dirty = true;
    }
}

bool dm_add_binding(device_record_t *dev, uint8_t src_endpoint,
                    const binding_record_t *binding)
{
    if (!dev || !binding) return false;

    for (int e = 0; e < dev->endpoint_count && e < MAX_ENDPOINTS; e++) {
        endpoint_record_t *ep = &dev->endpoints[e];
        if (ep->endpoint_id != src_endpoint) {
            continue;
        }
        for (int b = 0; b < ep->binding_count; b++) {
            if (memcmp(&ep->bindings[b], binding, sizeof(*binding)) == 0) {
                return true;
            }
        }
        if (ep->binding_count >= MAX_BINDINGS_PER_EP) {
            return false;
        }

        ep->bindings[ep->binding_count++] = *binding;
        dev->dirty = true;
        return true;
    }

    return false;
}
