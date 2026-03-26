#include "bridge_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "timebase.h"

static const char *TAG = "bridge_core";
static const char *BRIDGE_REG_NS = "bridge_reg";
static const char *BRIDGE_REG_KEY = "registry_v1";
static const uint32_t BRIDGE_REG_VER = 1U;

typedef struct {
    uint32_t version;
    uint16_t next_matter_endpoint_id;
    uint16_t reserved;
    bridge_device_binding_t bindings[DEVICE_TABLE_MAX_DEVICES];
} bridge_registry_storage_t;

static bridge_registry_storage_t s_storage;
static bridge_device_state_t s_runtime[DEVICE_TABLE_MAX_DEVICES];
static SemaphoreHandle_t s_bridge_mutex = NULL;
static bool s_loaded_from_nvs = false;
static bool s_dirty = false;
static bool s_sync_requested = true;

static bool bridge_lock(void)
{
    return s_bridge_mutex != NULL && xSemaphoreTakeRecursive(s_bridge_mutex, portMAX_DELAY) == pdTRUE;
}

static void bridge_unlock(void)
{
    if (s_bridge_mutex != NULL) {
        xSemaphoreGiveRecursive(s_bridge_mutex);
    }
}

static void bridge_storage_reset_locked(void)
{
    memset(&s_storage, 0, sizeof(s_storage));
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_storage.version = BRIDGE_REG_VER;
}

static uint32_t stable_matter_device_id(uint64_t ieee)
{
    uint32_t folded = (uint32_t)(ieee & 0xFFFFFFFFULL) ^ (uint32_t)(ieee >> 32);
    if (folded == 0U) {
        folded = 1U;
    }
    return folded;
}

static void make_matter_unique_id(uint64_t ieee, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    snprintf(out, out_size, "zigbee-%016" PRIx64, ieee);
}

static bool endpoint_has_cluster(const device_endpoint_record_t *ep, uint16_t cluster_id)
{
    if (ep == NULL) {
        return false;
    }
    for (size_t i = 0; i < ep->input_clusters_len; ++i) {
        if (ep->input_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static uint32_t compute_expose_mask(const device_record_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }

    uint32_t mask = 0U;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t *ep = &dev->endpoints[i];
        if (!ep->used) {
            continue;
        }
        if (ep->has_on_off || endpoint_has_cluster(ep, 0x0006)) {
            mask |= BRIDGE_EXPOSE_ON_OFF;
        }
        if (ep->has_temperature || endpoint_has_cluster(ep, 0x0402)) {
            mask |= BRIDGE_EXPOSE_TEMPERATURE;
        }
        if (ep->has_humidity || endpoint_has_cluster(ep, 0x0405)) {
            mask |= BRIDGE_EXPOSE_HUMIDITY;
        }
        if (ep->has_occupancy || endpoint_has_cluster(ep, 0x0406)) {
            mask |= BRIDGE_EXPOSE_OCCUPANCY;
        }
        if (ep->has_illuminance || endpoint_has_cluster(ep, 0x0400)) {
            mask |= BRIDGE_EXPOSE_ILLUMINANCE;
        }
        if (ep->has_pressure || endpoint_has_cluster(ep, 0x0403)) {
            mask |= BRIDGE_EXPOSE_PRESSURE;
        }
        if (ep->has_power_battery_pct || ep->has_power_battery_voltage || endpoint_has_cluster(ep, 0x0001)) {
            mask |= BRIDGE_EXPOSE_BATTERY;
        }
    }
    return mask;
}

static bool is_supported_mask(uint32_t mask)
{
    return (mask & (BRIDGE_EXPOSE_ON_OFF | BRIDGE_EXPOSE_TEMPERATURE | BRIDGE_EXPOSE_HUMIDITY |
                    BRIDGE_EXPOSE_OCCUPANCY | BRIDGE_EXPOSE_ILLUMINANCE)) != 0U;
}

static int find_binding_slot_by_ieee_locked(uint64_t ieee)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_storage.bindings[i].used != 0U && s_storage.bindings[i].zigbee_ieee == ieee) {
            return (int)i;
        }
    }
    return -1;
}

static int allocate_binding_slot_locked(void)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_storage.bindings[i].used == 0U) {
            return (int)i;
        }
    }
    return -1;
}

void bridge_core_format_friendly_name(const device_record_t *dev, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';
    if (dev == NULL) {
        snprintf(out, out_size, "device");
        return;
    }

    const char *sources[] = {dev->model, dev->norm_name, dev->manufacturer, "device"};
    size_t pos = 0;
    bool last_sep = true;

    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]) && pos == 0; ++s) {
        const unsigned char *p = (const unsigned char *)sources[s];
        while (*p != '\0' && pos + 1U < out_size) {
            const unsigned char ch = *p++;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
                out[pos++] = (char)ch;
                last_sep = false;
            } else if (ch >= 'A' && ch <= 'Z') {
                out[pos++] = (char)(ch - 'A' + 'a');
                last_sep = false;
            } else if (!last_sep && pos + 1U < out_size) {
                out[pos++] = '_';
                last_sep = true;
            }
        }
        while (pos > 0U && out[pos - 1U] == '_') {
            --pos;
        }
        out[pos] = '\0';
    }

    if (pos == 0U) {
        snprintf(out, out_size, "device");
        pos = strlen(out);
    }

    char suffix[12];
    snprintf(suffix, sizeof(suffix), "_%08" PRIx64, dev->ieee & 0xFFFFFFFFULL);
    if (pos + strlen(suffix) + 1U < out_size) {
        memcpy(out + pos, suffix, strlen(suffix) + 1U);
    }
}

static void load_registry_from_nvs(void)
{
    nvs_handle_t nvs = 0;
    size_t len = sizeof(s_storage);
    bridge_storage_reset_locked();
    s_loaded_from_nvs = false;

    esp_err_t err = nvs_open(BRIDGE_REG_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "[T+%07.3f] NVS '%s' ausente; se inicia registro bridge vacio", timebase_now_s(), BRIDGE_REG_NS);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS open '%s' fallo: %s", timebase_now_s(), BRIDGE_REG_NS, esp_err_to_name(err));
        return;
    }

    err = nvs_get_blob(nvs, BRIDGE_REG_KEY, &s_storage, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "[T+%07.3f] NVS leer '%s' fallo: %s", timebase_now_s(), BRIDGE_REG_KEY, esp_err_to_name(err));
        }
        bridge_storage_reset_locked();
        return;
    }
    if (len != sizeof(s_storage) || s_storage.version != BRIDGE_REG_VER) {
        ESP_LOGW(TAG, "[T+%07.3f] Registro bridge ignorado por tamano/version (len=%u ver=%" PRIu32 ")",
                 timebase_now_s(), (unsigned)len, s_storage.version);
        bridge_storage_reset_locked();
        return;
    }

    s_loaded_from_nvs = true;
    ESP_LOGI(TAG, "[T+%07.3f] Registro bridge cargado desde NVS (next_ep=%u)", timebase_now_s(),
             (unsigned)s_storage.next_matter_endpoint_id);
}

static void persist_registry_to_nvs(void)
{
    nvs_handle_t nvs = 0;

    esp_err_t err = nvs_open(BRIDGE_REG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] NVS open RW '%s' fallo: %s", timebase_now_s(), BRIDGE_REG_NS, esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, BRIDGE_REG_KEY, &s_storage, sizeof(s_storage));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T+%07.3f] Guardar registro bridge fallo: %s", timebase_now_s(), esp_err_to_name(err));
        return;
    }

    s_dirty = false;
    ESP_LOGI(TAG, "[T+%07.3f] Registro bridge persistido en NVS", timebase_now_s());
}

static void refresh_runtime_defaults_locked(void)
{
    memset(s_runtime, 0, sizeof(s_runtime));
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_storage.bindings[i].used == 0U) {
            continue;
        }
        s_runtime[i].used = 1U;
        s_runtime[i].supported = is_supported_mask(s_storage.bindings[i].expose_mask) ? 1U : 0U;
    }
}

void bridge_core_init(void)
{
    if (s_bridge_mutex == NULL) {
        s_bridge_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_bridge_mutex == NULL) {
            ESP_LOGE(TAG, "[T+%07.3f] No se pudo crear mutex bridge", timebase_now_s());
            return;
        }
    }
    if (!bridge_lock()) {
        return;
    }
    load_registry_from_nvs();
    refresh_runtime_defaults_locked();
    s_sync_requested = true;
    bridge_unlock();
}

void bridge_core_request_sync(void)
{
    s_sync_requested = true;
}

void bridge_core_poll(void)
{
    if (s_sync_requested) {
        device_record_t dev = {0};

        if (!bridge_lock()) {
            return;
        }
        refresh_runtime_defaults_locked();
        bridge_unlock();

        for (size_t slot = 0; slot < DEVICE_TABLE_MAX_DEVICES; ++slot) {
            if (!device_table_copy_device_at(slot, &dev) || !dev.occupied) {
                continue;
            }

            const uint32_t expose_mask = compute_expose_mask(&dev);
            const bool supported = is_supported_mask(expose_mask);
            char friendly_name[BRIDGE_FRIENDLY_NAME_MAX];
            char matter_unique_id[BRIDGE_MATTER_UNIQUE_ID_MAX];
            bridge_core_format_friendly_name(&dev, friendly_name, sizeof(friendly_name));
            make_matter_unique_id(dev.ieee, matter_unique_id, sizeof(matter_unique_id));

            if (!bridge_lock()) {
                return;
            }
            int binding_slot = find_binding_slot_by_ieee_locked(dev.ieee);
            if (binding_slot < 0) {
                binding_slot = allocate_binding_slot_locked();
                if (binding_slot >= 0) {
                    bridge_device_binding_t *binding = &s_storage.bindings[binding_slot];
                    memset(binding, 0, sizeof(*binding));
                    binding->used = 1U;
                    binding->zigbee_ieee = dev.ieee;
                    binding->zigbee_norm_type = (uint8_t)dev.norm_type;
                    binding->matter_device_id = stable_matter_device_id(dev.ieee);
                    binding->expose_mask = expose_mask;
                    memcpy(binding->friendly_name, friendly_name, sizeof(binding->friendly_name));
                    memcpy(binding->matter_unique_id, matter_unique_id, sizeof(binding->matter_unique_id));
                    s_dirty = true;
                    ESP_LOGI(TAG, "[T+%07.3f] Registro bridge: alta ieee=0x%016" PRIX64 " name=%s ep=%u supported=%s",
                             timebase_now_s(), dev.ieee, binding->friendly_name, (unsigned)binding->matter_endpoint_id,
                             supported ? "true" : "false");
                }
            }

            if (binding_slot >= 0) {
                bridge_device_binding_t *binding = &s_storage.bindings[binding_slot];
                bridge_device_state_t *state = &s_runtime[binding_slot];
                bool persistent_changed = false;

                if (binding->zigbee_norm_type != (uint8_t)dev.norm_type) {
                    binding->zigbee_norm_type = (uint8_t)dev.norm_type;
                    persistent_changed = true;
                }
                if (binding->expose_mask != expose_mask) {
                    binding->expose_mask = expose_mask;
                    persistent_changed = true;
                }
                if (binding->friendly_name[0] == '\0') {
                    memcpy(binding->friendly_name, friendly_name, sizeof(binding->friendly_name));
                    persistent_changed = true;
                }
                if (binding->matter_unique_id[0] == '\0') {
                    memcpy(binding->matter_unique_id, matter_unique_id, sizeof(binding->matter_unique_id));
                    persistent_changed = true;
                }
                if (binding->matter_device_id == 0U) {
                    binding->matter_device_id = stable_matter_device_id(dev.ieee);
                    persistent_changed = true;
                }
                if (persistent_changed) {
                    s_dirty = true;
                }

                state->used = 1U;
                state->supported = supported ? 1U : 0U;
                state->reachable = (dev.in_network && !dev.silent) ? 1U : 0U;
                state->in_network = dev.in_network ? 1U : 0U;
                state->zigbee_short_addr = dev.short_addr;
                state->silence_level = dev.silence_level;
                state->last_seen_s = dev.last_seen_s;
            }
            bridge_unlock();
        }

        s_sync_requested = false;
    }

    if (!bridge_lock()) {
        return;
    }
    if (s_dirty) {
        persist_registry_to_nvs();
    }
    bridge_unlock();
}

void bridge_core_reset_registry(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(BRIDGE_REG_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, BRIDGE_REG_KEY);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            (void)nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    if (!bridge_lock()) {
        return;
    }
    bridge_storage_reset_locked();
    s_loaded_from_nvs = false;
    s_dirty = false;
    s_sync_requested = true;
    bridge_unlock();

    ESP_LOGW(TAG, "[T+%07.3f] Registro bridge borrado", timebase_now_s());
}

void bridge_core_get_status(bridge_registry_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!bridge_lock()) {
        return;
    }
    out->version = s_storage.version;
    out->next_matter_endpoint_id = s_storage.next_matter_endpoint_id;
    out->loaded_from_nvs = s_loaded_from_nvs ? 1U : 0U;
    out->dirty = s_dirty ? 1U : 0U;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_storage.bindings[i].used != 0U) {
            out->active_bindings++;
        }
    }
    bridge_unlock();
}

bool bridge_core_copy_binding_at(size_t slot, bridge_device_binding_t *binding_out, bridge_device_state_t *state_out)
{
    if (slot >= DEVICE_TABLE_MAX_DEVICES) {
        return false;
    }
    if (!bridge_lock()) {
        return false;
    }
    const bool ok = (s_storage.bindings[slot].used != 0U);
    if (ok && binding_out != NULL) {
        *binding_out = s_storage.bindings[slot];
    }
    if (ok && state_out != NULL) {
        *state_out = s_runtime[slot];
    }
    bridge_unlock();
    return ok;
}

bool bridge_core_copy_binding_by_ieee(uint64_t ieee, bridge_device_binding_t *binding_out, bridge_device_state_t *state_out)
{
    if (!bridge_lock()) {
        return false;
    }
    const int slot = find_binding_slot_by_ieee_locked(ieee);
    const bool ok = (slot >= 0);
    if (ok && binding_out != NULL) {
        *binding_out = s_storage.bindings[slot];
    }
    if (ok && state_out != NULL) {
        *state_out = s_runtime[slot];
    }
    bridge_unlock();
    return ok;
}

bool bridge_core_set_matter_endpoint_id(uint64_t ieee, uint16_t matter_endpoint_id)
{
    if (!bridge_lock()) {
        return false;
    }
    const int slot = find_binding_slot_by_ieee_locked(ieee);
    if (slot < 0) {
        bridge_unlock();
        return false;
    }
    bridge_device_binding_t *binding = &s_storage.bindings[slot];
    if (binding->matter_endpoint_id == matter_endpoint_id) {
        bridge_unlock();
        return true;
    }
    binding->matter_endpoint_id = matter_endpoint_id;
    s_dirty = true;
    bridge_unlock();
    return true;
}

void bridge_core_dump_registry_json(void)
{
    bridge_registry_status_t status = {0};
    bridge_core_get_status(&status);

    printf("{\n");
    printf("  \"bridge_registry\": {\n");
    printf("    \"version\": %" PRIu32 ",\n", status.version);
    printf("    \"active_bindings\": %" PRIu32 ",\n", status.active_bindings);
    printf("    \"next_matter_endpoint_id\": %u,\n", (unsigned)status.next_matter_endpoint_id);
    printf("    \"loaded_from_nvs\": %s,\n", status.loaded_from_nvs ? "true" : "false");
    printf("    \"dirty\": %s,\n", status.dirty ? "true" : "false");
    printf("    \"devices\": [\n");

    bool first = true;
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        bridge_device_binding_t binding = {0};
        bridge_device_state_t state = {0};
        if (!bridge_core_copy_binding_at(i, &binding, &state)) {
            continue;
        }
        if (!first) {
            printf(",\n");
        }
        first = false;
        printf("      {\"slot\": %u, \"ieee\": \"0x%016" PRIX64 "\", \"friendly_name\": \"%s\", "
               "\"matter_unique_id\": \"%s\", \"matter_endpoint_id\": %u, \"reachable\": %s, "
               "\"in_network\": %s, \"supported\": %s, \"short\": \"0x%04X\", \"expose_mask\": %" PRIu32 "}",
               (unsigned)i, binding.zigbee_ieee, binding.friendly_name, binding.matter_unique_id,
               (unsigned)binding.matter_endpoint_id, state.reachable ? "true" : "false",
               state.in_network ? "true" : "false", state.supported ? "true" : "false",
               (unsigned)state.zigbee_short_addr, binding.expose_mask);
    }
    printf("\n    ]\n");
    printf("  }\n");
    printf("}\n");
}
