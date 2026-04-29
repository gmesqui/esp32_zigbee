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
    uint8_t  binding_count;
} nvs_ep_hdr_t;

typedef struct {
    uint16_t cluster_id;
    uint8_t  dst_addr_mode;
    uint16_t dst_group_addr;
    uint64_t dst_ieee_addr;
    uint8_t  dst_endpoint;
} nvs_binding_t;

#pragma pack(pop)

// Maximum blob size per device
#define MAX_BLOB_SIZE  (sizeof(nvs_dev_hdr_t) + \
                        MAX_ENDPOINTS * (sizeof(nvs_ep_hdr_t) + \
                        MAX_CLUSTERS_PER_EP * 2 * sizeof(uint16_t) + \
                        MAX_BINDINGS_PER_EP * sizeof(nvs_binding_t)))

static uint8_t s_blob[MAX_BLOB_SIZE];          // shared serialisation buffer
static uint8_t s_existing_blob[MAX_BLOB_SIZE]; // shared comparison buffer

// ---------------------------------------------------------------------------
// Serialise one device into s_blob. Returns written length.
// ---------------------------------------------------------------------------

static size_t serialise(uint8_t idx)
{
    device_record_t *d = dm_get_by_index(idx);
    if (!d) return 0;

    uint8_t *p = s_blob;

    bool descriptors_complete = dm_has_complete_descriptors(d);
    bool reporting_configured = d->reporting_configured && descriptors_complete;
    uint8_t persisted_endpoint_count = descriptors_complete ? d->endpoint_count : 0;

    nvs_dev_hdr_t hdr = {
        .cache_ver           = NVS_CACHE_VERSION,
        .ieee_addr           = d->ieee_addr,
        .nwk_addr            = d->nwk_addr,
        .node_desc_flags     = d->node_desc_flags,
        .mac_capability_flags= d->mac_capability_flags,
        .manufacturer_code   = d->manufacturer_code,
        .power_desc_flags    = d->power_desc_flags,
        .power_source        = d->power_source,
        .endpoint_count      = persisted_endpoint_count,
        .reporting_configured= reporting_configured,
    };
    strncpy(hdr.friendly_name, d->friendly_name, FRIENDLY_NAME_LEN);
    strncpy(hdr.manufacturer,  d->manufacturer,  32);
    strncpy(hdr.model,         d->model,         32);

    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (int e = 0; e < persisted_endpoint_count && e < MAX_ENDPOINTS; e++) {
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
            .binding_count    = (ep->binding_count < MAX_BINDINGS_PER_EP)
                                 ? ep->binding_count : MAX_BINDINGS_PER_EP,
        };
        memcpy(p, &ehdr, sizeof(ehdr));
        p += sizeof(ehdr);

        size_t in_bytes  = ehdr.in_cluster_count  * sizeof(uint16_t);
        size_t out_bytes = ehdr.out_cluster_count * sizeof(uint16_t);
        memcpy(p, ep->in_clusters,  in_bytes);  p += in_bytes;
        memcpy(p, ep->out_clusters, out_bytes); p += out_bytes;

        for (int b = 0; b < ehdr.binding_count; b++) {
            nvs_binding_t bhdr = {
                .cluster_id = ep->bindings[b].cluster_id,
                .dst_addr_mode = ep->bindings[b].dst_addr_mode,
                .dst_group_addr = ep->bindings[b].dst_group_addr,
                .dst_ieee_addr = ep->bindings[b].dst_ieee_addr,
                .dst_endpoint = ep->bindings[b].dst_endpoint,
            };
            memcpy(p, &bhdr, sizeof(bhdr));
            p += sizeof(bhdr);
        }
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
    bool cache_reporting_configured = hdr->reporting_configured;
    uint8_t saved_endpoint_count = (hdr->endpoint_count < MAX_ENDPOINTS)
                                    ? hdr->endpoint_count : MAX_ENDPOINTS;
    d->reporting_configured = cache_reporting_configured;
    d->endpoint_count       = 0;
    d->is_sleepy            = !(hdr->mac_capability_flags & 0x08);

    // Online=false until we hear from the device
    d->online = false;
    d->dirty  = false;

    p += sizeof(nvs_dev_hdr_t);

    // Restore endpoints. If the blob is truncated, keep only complete entries.
    for (int e = 0; e < saved_endpoint_count; e++) {
        if ((size_t)(p - blob) + sizeof(nvs_ep_hdr_t) > len) break;

        const nvs_ep_hdr_t *ehdr = (const nvs_ep_hdr_t *)p;
        p += sizeof(nvs_ep_hdr_t);

        endpoint_record_t *ep = &d->endpoints[d->endpoint_count];
        ep->endpoint_id      = ehdr->endpoint_id;
        ep->profile_id       = ehdr->profile_id;
        ep->device_id        = ehdr->device_id;
        ep->device_version   = ehdr->device_version;
        ep->in_cluster_count = (ehdr->in_cluster_count < MAX_CLUSTERS_PER_EP)
                                ? ehdr->in_cluster_count : MAX_CLUSTERS_PER_EP;
        ep->out_cluster_count= (ehdr->out_cluster_count < MAX_CLUSTERS_PER_EP)
                                ? ehdr->out_cluster_count : MAX_CLUSTERS_PER_EP;
        ep->binding_count    = (ehdr->binding_count < MAX_BINDINGS_PER_EP)
                                ? ehdr->binding_count : MAX_BINDINGS_PER_EP;

        size_t in_bytes  = ep->in_cluster_count  * sizeof(uint16_t);
        size_t out_bytes = ep->out_cluster_count * sizeof(uint16_t);

        if ((size_t)(p - blob) + in_bytes + out_bytes > len) break;
        memcpy(ep->in_clusters,  p, in_bytes);  p += in_bytes;
        memcpy(ep->out_clusters, p, out_bytes); p += out_bytes;

        size_t binding_bytes = ep->binding_count * sizeof(nvs_binding_t);
        if ((size_t)(p - blob) + binding_bytes > len) {
            ep->binding_count = 0;
            break;
        }

        for (int b = 0; b < ep->binding_count; b++) {
            const nvs_binding_t *bhdr = (const nvs_binding_t *)p;
            ep->bindings[b].cluster_id = bhdr->cluster_id;
            ep->bindings[b].dst_addr_mode = bhdr->dst_addr_mode;
            ep->bindings[b].dst_group_addr = bhdr->dst_group_addr;
            ep->bindings[b].dst_ieee_addr = bhdr->dst_ieee_addr;
            ep->bindings[b].dst_endpoint = bhdr->dst_endpoint;
            p += sizeof(nvs_binding_t);
        }

        d->endpoint_count++;
    }

    bool descriptors_valid =
        (d->endpoint_count == saved_endpoint_count &&
         dm_has_complete_descriptors(d));
    if (!descriptors_valid) {
        if (cache_reporting_configured) {
            char ibuf[20];
            utils_ieee_to_str(hdr->ieee_addr, ibuf, sizeof(ibuf));
            ZB_LOG("NVS cache: %s descriptors incomplete; forcing re-interview",
                   ibuf);
        }
        d->reporting_configured = false;
        d->state = DEV_STATE_NEW;
        d->dirty = true;
    } else if (d->state < DEV_STATE_INTERVIEWED) {
        d->state = d->reporting_configured ? DEV_STATE_CONFIGURED
                                           : DEV_STATE_INTERVIEWED;
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

static void build_head(uint8_t head[2])
{
    head[0] = NVS_CACHE_VERSION;
    head[1] = (uint8_t)dm_count();
}

static bool nvs_blob_matches(nvs_handle_t handle, const char *key,
                             const uint8_t *blob, size_t len)
{
    size_t existing_len = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &existing_len);
    if (err != ESP_OK || existing_len != len || existing_len > sizeof(s_existing_blob)) {
        return false;
    }

    err = nvs_get_blob(handle, key, s_existing_blob, &existing_len);
    return err == ESP_OK && existing_len == len &&
           memcmp(s_existing_blob, blob, len) == 0;
}

static esp_err_t write_head_if_changed(nvs_handle_t handle, bool *changed)
{
    uint8_t head[2];
    uint8_t existing[2] = {0};
    size_t existing_len = sizeof(existing);
    esp_err_t err;

    if (changed) {
        *changed = false;
    }

    build_head(head);
    err = nvs_get_blob(handle, NVS_KEY_HEAD, existing, &existing_len);
    if (err == ESP_OK && existing_len == sizeof(existing) &&
        memcmp(existing, head, sizeof(head)) == 0) {
        return ESP_OK;
    }

    err = nvs_set_blob(handle, NVS_KEY_HEAD, head, sizeof(head));
    if (err == ESP_OK && changed) {
        *changed = true;
    }
    return err;
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

    bool needs_commit = false;
    bool device_written = false;
    bool header_changed = false;
    bool header_error = false;
    size_t len = serialise(idx);
    if (len > 0) {
        char key[16];
        make_key(idx, key, sizeof(key));
        if (!nvs_blob_matches(handle, key, s_blob, len)) {
            err = nvs_set_blob(handle, key, s_blob, len);
            if (err == ESP_OK) {
                needs_commit = true;
                device_written = true;
            } else {
                ZB_LOG("NVS cache: write error for slot %u (%d)", idx, err);
            }
        }
    }

    // Update header
    err = write_head_if_changed(handle, &header_changed);
    if (err != ESP_OK) {
        ZB_LOG("NVS cache: header update failed (%d)", err);
        header_error = true;
    } else if (header_changed) {
        needs_commit = true;
    }

    if (needs_commit) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            d->dirty = header_error;
            if (!device_written && header_changed) {
                ZB_LOG("NVS cache: header updated");
            }
        } else {
            d->dirty = true;
            ZB_LOG("NVS cache: commit failed for slot %u (%d)", idx, err);
        }
    } else if (!header_error) {
        d->dirty = false;
    }
    nvs_close(handle);
}

void nvs_cache_delete_device(uint8_t idx)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ZB_LOG("NVS cache: open for delete failed (%d)", err);
        return;
    }

    char key[16];
    make_key(idx, key, sizeof(key));
    bool needs_commit = false;
    bool header_changed = false;
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        needs_commit = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ZB_LOG("NVS cache: erase error for slot %u (%d)", idx, err);
    }

    err = write_head_if_changed(handle, &header_changed);
    if (err != ESP_OK) {
        ZB_LOG("NVS cache: header update failed after delete (%d)", err);
    } else if (header_changed) {
        needs_commit = true;
    }

    if (needs_commit) {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ZB_LOG("NVS cache: commit failed after delete (%d)", err);
        } else {
            ZB_LOG("NVS cache: deleted slot %u", idx);
        }
    }
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
