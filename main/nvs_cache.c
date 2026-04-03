#include "nvs_cache.h"
#include "device_manager.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"

// ---------------------------------------------------------------------------
// Serialised device layout (packed, version-guarded)
// Variable-length: header + endpoints + cluster lists
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

typedef struct {
    uint8_t  cache_ver;
    uint64_t ieee_addr;
    uint16_t nwk_addr;
    char     friendly_name[FRIENDLY_NAME_LEN];
    uint16_t node_desc_flags;
    uint8_t  mac_capability_flags;
    uint16_t manufacturer_code;
    uint16_t power_desc_flags;
    char     manufacturer[33];
    char     model[33];
    uint8_t  power_source;
    uint8_t  endpoint_count;
    bool     reporting_configured;
    // followed by endpoint_count * nvs_ep_hdr_t records,
    // each immediately followed by (in_count + out_count) * uint16_t cluster IDs
} nvs_dev_hdr_t;

typedef struct {
    uint8_t  endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t  device_version;
    uint8_t  in_cluster_count;
    uint8_t  out_cluster_count;
} nvs_ep_hdr_t;

#pragma pack(pop)

// Maximum blob size per device
#define MAX_BLOB_SIZE  (sizeof(nvs_dev_hdr_t) + \
                        MAX_ENDPOINTS * (sizeof(nvs_ep_hdr_t) + \
                        MAX_CLUSTERS_PER_EP * 2 * sizeof(uint16_t)))

static uint8_t s_blob[MAX_BLOB_SIZE];   // shared serialisation buffer

// ---------------------------------------------------------------------------
// Serialise one device into s_blob. Returns written length.
// ---------------------------------------------------------------------------

static size_t serialise(uint8_t idx)
{
    device_record_t *d = dm_get_by_index(idx);
    if (!d) return 0;

    uint8_t *p = s_blob;

    nvs_dev_hdr_t hdr = {
        .cache_ver           = NVS_CACHE_VERSION,
        .ieee_addr           = d->ieee_addr,
        .nwk_addr            = d->nwk_addr,
        .node_desc_flags     = d->node_desc_flags,
        .mac_capability_flags= d->mac_capability_flags,
        .manufacturer_code   = d->manufacturer_code,
        .power_desc_flags    = d->power_desc_flags,
        .power_source        = d->power_source,
        .endpoint_count      = d->endpoint_count,
        .reporting_configured= d->reporting_configured,
    };
    strncpy(hdr.friendly_name, d->friendly_name, FRIENDLY_NAME_LEN);
    strncpy(hdr.manufacturer,  d->manufacturer,  32);
    strncpy(hdr.model,         d->model,         32);

    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (int e = 0; e < d->endpoint_count && e < MAX_ENDPOINTS; e++) {
        endpoint_record_t *ep = &d->endpoints[e];
        nvs_ep_hdr_t ehdr = {
            .endpoint_id     = ep->endpoint_id,
            .profile_id      = ep->profile_id,
            .device_id       = ep->device_id,
            .device_version  = ep->device_version,
            .in_cluster_count = (ep->in_cluster_count < MAX_CLUSTERS_PER_EP)
                                 ? ep->in_cluster_count : MAX_CLUSTERS_PER_EP,
            .out_cluster_count= (ep->out_cluster_count < MAX_CLUSTERS_PER_EP)
                                 ? ep->out_cluster_count : MAX_CLUSTERS_PER_EP,
        };
        memcpy(p, &ehdr, sizeof(ehdr));
        p += sizeof(ehdr);

        size_t in_bytes  = ehdr.in_cluster_count  * sizeof(uint16_t);
        size_t out_bytes = ehdr.out_cluster_count * sizeof(uint16_t);
        memcpy(p, ep->in_clusters,  in_bytes);  p += in_bytes;
        memcpy(p, ep->out_clusters, out_bytes); p += out_bytes;
    }

    return (size_t)(p - s_blob);
}

// ---------------------------------------------------------------------------
// Deserialise from s_blob into the device table. Returns true on success.
// ---------------------------------------------------------------------------

static bool deserialise(const uint8_t *blob, size_t len)
{
    if (len < sizeof(nvs_dev_hdr_t)) return false;

    const uint8_t *p = blob;
    const nvs_dev_hdr_t *hdr = (const nvs_dev_hdr_t *)p;

    if (hdr->cache_ver != NVS_CACHE_VERSION) {
        ZB_LOG("NVS cache version mismatch (%u != %u) — skipping",
               hdr->cache_ver, NVS_CACHE_VERSION);
        return false;
    }

    // Get or create device entry
    device_record_t *d = dm_get_or_create(hdr->ieee_addr, hdr->nwk_addr);
    if (!d) return false;

    // Restore identity
    strncpy(d->friendly_name, hdr->friendly_name, FRIENDLY_NAME_LEN);
    strncpy(d->manufacturer,  hdr->manufacturer,  32);
    strncpy(d->model,         hdr->model,         32);
    d->node_desc_flags      = hdr->node_desc_flags;
    d->mac_capability_flags = hdr->mac_capability_flags;
    d->manufacturer_code    = hdr->manufacturer_code;
    d->power_desc_flags     = hdr->power_desc_flags;
    d->power_source         = hdr->power_source;
    d->reporting_configured = hdr->reporting_configured;
    d->endpoint_count       = (hdr->endpoint_count < MAX_ENDPOINTS)
                               ? hdr->endpoint_count : MAX_ENDPOINTS;
    d->is_sleepy            = !(hdr->mac_capability_flags & 0x08);

    // Restore state as INTERVIEWED (we know descriptors, just need runtime contact)
    if (d->state < DEV_STATE_INTERVIEWED) {
        d->state = d->reporting_configured ? DEV_STATE_CONFIGURED
                                           : DEV_STATE_INTERVIEWED;
    }

    // Online=false until we hear from the device
    d->online = false;
    d->dirty  = false;

    p += sizeof(nvs_dev_hdr_t);

    // Restore endpoints
    for (int e = 0; e < d->endpoint_count; e++) {
        if ((size_t)(p - blob) + sizeof(nvs_ep_hdr_t) > len) break;

        const nvs_ep_hdr_t *ehdr = (const nvs_ep_hdr_t *)p;
        p += sizeof(nvs_ep_hdr_t);

        endpoint_record_t *ep = &d->endpoints[e];
        ep->endpoint_id      = ehdr->endpoint_id;
        ep->profile_id       = ehdr->profile_id;
        ep->device_id        = ehdr->device_id;
        ep->device_version   = ehdr->device_version;
        ep->in_cluster_count = (ehdr->in_cluster_count < MAX_CLUSTERS_PER_EP)
                                ? ehdr->in_cluster_count : MAX_CLUSTERS_PER_EP;
        ep->out_cluster_count= (ehdr->out_cluster_count < MAX_CLUSTERS_PER_EP)
                                ? ehdr->out_cluster_count : MAX_CLUSTERS_PER_EP;

        size_t in_bytes  = ep->in_cluster_count  * sizeof(uint16_t);
        size_t out_bytes = ep->out_cluster_count * sizeof(uint16_t);

        if ((size_t)(p - blob) + in_bytes + out_bytes > len) break;
        memcpy(ep->in_clusters,  p, in_bytes);  p += in_bytes;
        memcpy(ep->out_clusters, p, out_bytes); p += out_bytes;
    }

    return true;
}

// ---------------------------------------------------------------------------
// NVS key for device slot
// ---------------------------------------------------------------------------

static void make_key(uint8_t idx, char *key, size_t key_len)
{
    snprintf(key, key_len, "%s%02u", NVS_KEY_DEV_PREFIX, (unsigned)idx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool nvs_cache_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ZB_LOG("NVS cache: namespace not found, starting fresh");
        return false;
    }

    // Read header
    uint8_t head[2] = {0};
    size_t head_len = sizeof(head);
    nvs_get_blob(handle, NVS_KEY_HEAD, head, &head_len);
    uint8_t ver   = head[0];
    uint8_t count = head[1];

    if (ver != NVS_CACHE_VERSION) {
        ZB_LOG("NVS cache: header version mismatch (%u) — starting fresh", ver);
        nvs_close(handle);
        return false;
    }

    ZB_LOG("NVS cache: loading %u device(s)", count);
    int loaded = 0;

    for (int i = 0; i < MAX_DEVICES; i++) {
        char key[16];
        make_key(i, key, sizeof(key));
        size_t blob_len = MAX_BLOB_SIZE;
        err = nvs_get_blob(handle, key, s_blob, &blob_len);
        if (err == ESP_OK) {
            if (deserialise(s_blob, blob_len)) {
                loaded++;
            }
        }
    }

    nvs_close(handle);
    ZB_LOG("NVS cache: loaded %d device(s)", loaded);
    return loaded > 0;
}

void nvs_cache_save_device(uint8_t idx)
{
    device_record_t *d = dm_get_by_index(idx);
    if (!d) return;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ZB_LOG("NVS cache: open for write failed (%d)", err);
        return;
    }

    size_t len = serialise(idx);
    if (len > 0) {
        char key[16];
        make_key(idx, key, sizeof(key));
        err = nvs_set_blob(handle, key, s_blob, len);
        if (err == ESP_OK) {
            nvs_commit(handle);
            d->dirty = false;
        } else {
            ZB_LOG("NVS cache: write error for slot %u (%d)", idx, err);
        }
    }

    // Update header
    uint8_t head[2] = { NVS_CACHE_VERSION, (uint8_t)dm_count() };
    nvs_set_blob(handle, NVS_KEY_HEAD, head, sizeof(head));
    nvs_commit(handle);
    nvs_close(handle);
}

void nvs_cache_save_dirty(void)
{
    bool any = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_record_t *d = dm_get_by_index(i);
        if (d && d->dirty) {
            nvs_cache_save_device((uint8_t)i);
            any = true;
        }
    }
    if (any) {
        ZB_LOG("NVS cache: dirty flush complete");
    }
}

void nvs_cache_erase(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ZB_LOG("NVS cache: erased");
    }
}
