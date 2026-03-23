#include "device_table.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "timebase.h"

static const char *TAG = "device_table";
static device_record_t s_devices[DEVICE_TABLE_MAX_DEVICES];

static device_record_t *find_by_ieee(uint64_t ieee)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].ieee == ieee) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_record_t *find_by_short(uint16_t short_addr)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].occupied && s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_record_t *pick_slot(void)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!s_devices[i].occupied) {
            return &s_devices[i];
        }
    }

    size_t oldest_idx = 0;
    double oldest = s_devices[0].last_seen_s;
    for (size_t i = 1; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_devices[i].last_seen_s < oldest) {
            oldest = s_devices[i].last_seen_s;
            oldest_idx = i;
        }
    }
    return &s_devices[oldest_idx];
}

static device_record_t *ensure_device(uint64_t ieee)
{
    device_record_t *rec = find_by_ieee(ieee);
    if (rec != NULL) {
        return rec;
    }
    rec = pick_slot();
    memset(rec, 0, sizeof(*rec));
    rec->occupied = true;
    rec->ieee = ieee;
    return rec;
}

static bool push_unique_u8(uint8_t *arr, size_t *len, size_t max, uint8_t v)
{
    for (size_t i = 0; i < *len; ++i) {
        if (arr[i] == v) {
            return false;
        }
    }
    if (*len >= max) {
        return false;
    }
    arr[*len] = v;
    (*len)++;
    return true;
}

static bool push_unique_u16(uint16_t *arr, size_t *len, size_t max, uint16_t v)
{
    for (size_t i = 0; i < *len; ++i) {
        if (arr[i] == v) {
            return false;
        }
    }
    if (*len >= max) {
        return false;
    }
    arr[*len] = v;
    (*len)++;
    return true;
}

void device_table_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
}

void device_table_touch(uint64_t ieee, uint16_t short_addr, int8_t rssi, uint8_t lqi)
{
    device_record_t *rec = ensure_device(ieee);
    rec->short_addr = short_addr;
    rec->rssi = rssi;
    rec->lqi = lqi;
    rec->last_seen_s = timebase_now_s();
}

void device_table_update_discovery(uint64_t ieee, uint16_t short_addr, uint16_t device_id, const char *manufacturer, const char *model)
{
    device_record_t *rec = ensure_device(ieee);
    rec->short_addr = short_addr;
    rec->device_id = device_id;
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL) {
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL) {
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
}

void device_table_update_from_trace(const zb_trace_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    /* No crear entradas con IEEE sintetico: solo actualizar dispositivos ya descubiertos por ZDO. */
    device_record_t *rec = find_by_short(meta->src_short);
    if (rec == NULL) {
        return;
    }
    rec->rssi = meta->rssi;
    rec->lqi = meta->lqi;
    rec->last_seen_s = timebase_now_s();
}

void device_table_update_identity(uint16_t short_addr, const char *manufacturer, const char *model)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }
    rec->last_seen_s = timebase_now_s();
    if (manufacturer != NULL && manufacturer[0] != '\0') {
        snprintf(rec->manufacturer, sizeof(rec->manufacturer), "%s", manufacturer);
    }
    if (model != NULL && model[0] != '\0') {
        snprintf(rec->model, sizeof(rec->model), "%s", model);
    }
}

void device_table_update_node_desc(uint16_t short_addr, uint16_t manufacturer_code, uint8_t mac_capability_flags)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }
    rec->last_seen_s = timebase_now_s();
    rec->state_flags = ((uint32_t)manufacturer_code << 8) | (uint32_t)mac_capability_flags;
}

void device_table_update_simple_desc(uint16_t short_addr, uint8_t endpoint, uint16_t device_id, const uint16_t *clusters_in,
                                     size_t clusters_in_len, const uint16_t *clusters_out, size_t clusters_out_len)
{
    device_record_t *rec = find_by_short(short_addr);
    if (rec == NULL) {
        const uint64_t ieee_key = 0x00000000FFFF0000ULL | (uint64_t)short_addr;
        rec = ensure_device(ieee_key);
        rec->short_addr = short_addr;
    }

    rec->device_id = device_id;
    rec->last_seen_s = timebase_now_s();
    (void)push_unique_u8(rec->endpoints, &rec->endpoints_len, DEVICE_TABLE_MAX_ENDPOINTS, endpoint);

    if (clusters_in != NULL) {
        for (size_t i = 0; i < clusters_in_len; ++i) {
            (void)push_unique_u16(rec->clusters_in, &rec->clusters_in_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_in[i]);
        }
    }
    if (clusters_out != NULL) {
        for (size_t i = 0; i < clusters_out_len; ++i) {
            (void)push_unique_u16(rec->clusters_out, &rec->clusters_out_len, DEVICE_TABLE_MAX_CLUSTERS, clusters_out[i]);
        }
    }
}

void device_table_dump_json(void)
{
    printf("{\n");
    printf("  \"ts_s\": %.3f,\n", timebase_now_s());
    printf("  \"devices\": [\n");
    bool first = true;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        const device_record_t *d = &s_devices[i];
        if (!d->occupied) {
            continue;
        }
        printf("%s", first ? "" : ",\n");
        first = false;
        printf("    {\n");
        printf("      \"ieee\": \"0x%016" PRIX64 "\",\n", d->ieee);
        printf("      \"short\": \"0x%04X\",\n", d->short_addr);
        printf("      \"device_id\": \"0x%04X\",\n", d->device_id);
        printf("      \"last_seen_s\": %.3f,\n", d->last_seen_s);
        printf("      \"lqi\": %u,\n", d->lqi);
        printf("      \"rssi\": %d,\n", d->rssi);
        printf("      \"manufacturer\": \"%s\",\n", d->manufacturer);
        printf("      \"model\": \"%s\",\n", d->model);
        printf("      \"state_flags\": %u,\n", (unsigned)d->state_flags);

        printf("      \"endpoints\": [");
        for (size_t e = 0; e < d->endpoints_len; ++e) {
            printf("%s%u", (e == 0) ? "" : ",", d->endpoints[e]);
        }
        printf("],\n");

        printf("      \"clusters_in\": [");
        for (size_t c = 0; c < d->clusters_in_len; ++c) {
            printf("%s\"0x%04X\"", (c == 0) ? "" : ",", d->clusters_in[c]);
        }
        printf("],\n");

        printf("      \"clusters_out\": [");
        for (size_t c = 0; c < d->clusters_out_len; ++c) {
            printf("%s\"0x%04X\"", (c == 0) ? "" : ",", d->clusters_out[c]);
        }
        printf("]\n");
        printf("    }");
    }
    printf("\n  ]\n");
    printf("}\n");
    ESP_LOGI(TAG, "[T+%07.3f] JSON de dispositivos emitido", timebase_now_s());
}
