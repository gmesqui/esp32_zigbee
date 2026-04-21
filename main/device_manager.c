#include "device_manager.h"
#include "zb_events.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// Global device table — static allocation
// ---------------------------------------------------------------------------

static device_record_t g_devices[MAX_DEVICES];
static uint8_t         g_count = 0;
static SemaphoreHandle_t g_mutex = NULL;

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

// Offline thresholds for inactivity-based presence fallback.
// Shorter values make stale availability decay faster when no stack signal arrives.
#define OFFLINE_THRESHOLD_ALWAYS_ON_MS  (300u  * 1000u)
#define OFFLINE_THRESHOLD_SLEEPY_MS     (3600u * 1000u)

void dm_check_presence(void)
{
    uint32_t now = utils_uptime_ms();
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = &g_devices[i];
        if (!d->in_use || !d->online) continue;
        if (d->last_seen_ms == 0) continue;

        uint32_t threshold = d->is_sleepy
            ? OFFLINE_THRESHOLD_SLEEPY_MS
            : OFFLINE_THRESHOLD_ALWAYS_ON_MS;

        if ((now - d->last_seen_ms) > threshold) {
            d->online = false;
            uint32_t secs = (now - d->last_seen_ms) / 1000u;
            ZB_LOG("DEVICE %s OFFLINE (no contact %lu s)", dm_display_name(d), (unsigned long)secs);
            dm_emit_availability(d);
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

void dm_clear_bindings(device_record_t *dev)
{
    if (!dev) return;

    for (int e = 0; e < dev->endpoint_count && e < MAX_ENDPOINTS; e++) {
        dev->endpoints[e].binding_count = 0;
        memset(dev->endpoints[e].bindings, 0, sizeof(dev->endpoints[e].bindings));
    }
    dev->dirty = true;
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
        if (ep->binding_count >= MAX_BINDINGS_PER_EP) {
            return false;
        }

        ep->bindings[ep->binding_count++] = *binding;
        dev->dirty = true;
        return true;
    }

    return false;
}
