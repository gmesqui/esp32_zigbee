#include "client_actions.h"

#include "button_handler.h"
#include "device_interview.h"
#include "device_manager.h"
#include "nvs_cache.h"
#include "report_config.h"
#include "utils.h"

#include "freertos/FreeRTOS.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include <string.h>

#define CLIENT_ZB_OP_MAX 16
#define CLIENT_ZB_LOCK_WAIT_MS 1000

typedef enum {
    CLIENT_ZB_OP_NONE = 0,
    CLIENT_ZB_OP_SET_STATE,
    CLIENT_ZB_OP_SET_BRIGHTNESS,
    CLIENT_ZB_OP_READ_ATTR,
} client_zb_op_type_t;

typedef struct {
    bool in_use;
    client_zb_op_type_t type;
    uint8_t dev_idx;
    uint16_t generation;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t on_off_cmd_id;
    uint8_t level;
} client_zb_op_t;

typedef struct {
    const char *name;
    uint16_t cluster_id;
    uint16_t attr_id;
} attr_name_map_t;

static client_zb_op_t s_zb_ops[CLIENT_ZB_OP_MAX];

static bool zb_lock_for_client_api(void)
{
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(CLIENT_ZB_LOCK_WAIT_MS))) {
        return true;
    }

    ZB_LOG("CLIENT Zigbee lock timeout");
    return false;
}

static const attr_name_map_t k_attr_name_map[] = {
    { "state",       0x0006, 0x0000 },
    { "brightness",  0x0008, 0x0000 },
    { "temperature", 0x0402, 0x0000 },
    { "humidity",    0x0405, 0x0000 },
    { "pressure",    0x0403, 0x0000 },
    { "illuminance", 0x0400, 0x0000 },
    { "occupancy",   0x0406, 0x0000 },
    { "battery",     0x0001, 0x0021 },
    { "voltage",     0x0001, 0x0020 },
    { "power",       0x0B04, 0x050B },
    { "contact",     0x0500, 0x0002 },
    { "tamper",      0x0500, 0x0002 },
    { "battery_low", 0x0500, 0x0002 },
};

static void client_zb_op_alarm(uint8_t slot_idx);

static void trim_copy(const char *src, char *dst, size_t dst_len)
{
    const char *start = src ? src : "";
    const char *end;
    size_t len;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    end = start + strlen(start);
    while (end > start) {
        char ch = end[-1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        end--;
    }

    len = (size_t)(end - start);
    if (dst_len == 0) {
        return;
    }
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
}

static device_record_t *find_device_by_id(const char *id)
{
    uint64_t ieee = 0;
    device_record_t *dev;

    if (!id || id[0] == '\0') {
        return NULL;
    }

    dev = dm_find_by_friendly_name(id);
    if (dev) {
        return dev;
    }

    if (utils_str_to_ieee(id, &ieee)) {
        return dm_find_by_ieee(ieee);
    }

    return NULL;
}

static const attr_name_map_t *find_attr_name_map(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(k_attr_name_map) / sizeof(k_attr_name_map[0]); i++) {
        if (strcmp(k_attr_name_map[i].name, name) == 0) {
            return &k_attr_name_map[i];
        }
    }

    return NULL;
}

static bool schedule_zb_op(const client_zb_op_t *op, uint32_t delay_ms)
{
    if (!op) {
        return false;
    }

    if (!zb_lock_for_client_api()) {
        return false;
    }

    for (int idx = 0; idx < CLIENT_ZB_OP_MAX; idx++) {
        if (!s_zb_ops[idx].in_use) {
            s_zb_ops[idx] = *op;
            s_zb_ops[idx].in_use = true;
            esp_zb_scheduler_alarm(client_zb_op_alarm, (uint8_t)idx, delay_ms);
            esp_zb_lock_release();
            return true;
        }
    }

    esp_zb_lock_release();
    return false;
}

static void clear_zb_op(uint8_t slot_idx)
{
    if (slot_idx >= CLIENT_ZB_OP_MAX) {
        return;
    }
    memset(&s_zb_ops[slot_idx], 0, sizeof(s_zb_ops[slot_idx]));
}

static void client_zb_op_alarm(uint8_t slot_idx)
{
    client_zb_op_t *op;
    device_record_t *dev;

    if (slot_idx >= CLIENT_ZB_OP_MAX) {
        return;
    }

    op = &s_zb_ops[slot_idx];
    if (!op->in_use) {
        return;
    }

    dev = dm_get_by_index_generation(op->dev_idx, op->generation);
    if (!dev || !dev->in_use) {
        clear_zb_op(slot_idx);
        return;
    }

    switch (op->type) {
        case CLIENT_ZB_OP_SET_STATE:
        {
            esp_zb_zcl_on_off_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .src_endpoint = COORD_ENDPOINT,
                    .dst_addr_u.addr_short = dev->nwk_addr,
                    .dst_endpoint = op->endpoint,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .on_off_cmd_id = op->on_off_cmd_id,
            };
            ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s cmd=0x%02X",
                   dev->nwk_addr, op->endpoint, utils_cluster_name(0x0006),
                   op->on_off_cmd_id);
            ZB_LOG("CLIENT SET state -> %s ep=%u cmd=0x%02X",
                   dm_display_name(dev), op->endpoint, op->on_off_cmd_id);
            esp_zb_zcl_on_off_cmd_req(&cmd);
            break;
        }

        case CLIENT_ZB_OP_SET_BRIGHTNESS:
        {
            esp_zb_zcl_move_to_level_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .src_endpoint = COORD_ENDPOINT,
                    .dst_addr_u.addr_short = dev->nwk_addr,
                    .dst_endpoint = op->endpoint,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .level = op->level,
                .transition_time = 0,
            };
            ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s level=%u transition=%u",
                   dev->nwk_addr, op->endpoint, utils_cluster_name(0x0008),
                   op->level, 0u);
            ZB_LOG("CLIENT SET brightness -> %s ep=%u level=%u",
                   dm_display_name(dev), op->endpoint, op->level);
            esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&cmd);
            break;
        }

        case CLIENT_ZB_OP_READ_ATTR:
        {
            uint16_t attr = op->attr_id;
            esp_zb_zcl_read_attr_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .src_endpoint = COORD_ENDPOINT,
                    .dst_addr_u.addr_short = dev->nwk_addr,
                    .dst_endpoint = op->endpoint,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .clusterID = op->cluster_id,
                .attr_number = 1,
                .attr_field = &attr,
            };
            ZB_LOG("TX RAW dst=0x%04X ep=%u cluster=%s read_attrs=[0x%04X]",
                   dev->nwk_addr, op->endpoint, utils_cluster_name(op->cluster_id),
                   op->attr_id);
            ZB_LOG("CLIENT GET -> %s ep=%u cluster=%s attr=0x%04X",
                   dm_display_name(dev), op->endpoint,
                   utils_cluster_name(op->cluster_id), op->attr_id);
            esp_zb_zcl_read_attr_cmd_req(&cmd);
            break;
        }

        default:
            break;
    }

    clear_zb_op(slot_idx);
}

client_action_result_t client_actions_set_state(const char *device_id,
                                                const char *state,
                                                uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    client_zb_op_t op = {0};
    device_record_t *dev = find_device_by_id(device_id);

    if (!dev || !state || state[0] == '\0') {
        return !dev ? CLIENT_ACTION_DEVICE_NOT_FOUND : CLIENT_ACTION_INVALID_ARG;
    }
    if (!dm_has_in_cluster(dev, 0x0006, &endpoint)) {
        return CLIENT_ACTION_UNSUPPORTED;
    }

    op.type = CLIENT_ZB_OP_SET_STATE;
    op.dev_idx = (uint8_t)dm_index_of(dev);
    op.generation = dm_slot_generation(op.dev_idx);
    op.endpoint = endpoint;

    if (strcmp(state, "ON") == 0) {
        op.on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID;
    } else if (strcmp(state, "OFF") == 0) {
        op.on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    } else if (strcmp(state, "TOGGLE") == 0) {
        op.on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID;
    } else {
        return CLIENT_ACTION_INVALID_ARG;
    }

    return schedule_zb_op(&op, delay_ms) ? CLIENT_ACTION_OK : CLIENT_ACTION_BUSY;
}

client_action_result_t client_actions_set_brightness(const char *device_id,
                                                     int brightness,
                                                     uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    client_zb_op_t op = {0};
    device_record_t *dev = find_device_by_id(device_id);

    if (!dev) {
        return CLIENT_ACTION_DEVICE_NOT_FOUND;
    }
    if (brightness < 0 || brightness > 254) {
        return CLIENT_ACTION_INVALID_ARG;
    }
    if (!dm_has_in_cluster(dev, 0x0008, &endpoint)) {
        return CLIENT_ACTION_UNSUPPORTED;
    }

    op.type = CLIENT_ZB_OP_SET_BRIGHTNESS;
    op.dev_idx = (uint8_t)dm_index_of(dev);
    op.generation = dm_slot_generation(op.dev_idx);
    op.endpoint = endpoint;
    op.level = (uint8_t)brightness;

    return schedule_zb_op(&op, delay_ms) ? CLIENT_ACTION_OK : CLIENT_ACTION_BUSY;
}

client_action_result_t client_actions_read_attribute(const char *device_id,
                                                     uint16_t cluster_id,
                                                     uint16_t attr_id,
                                                     uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    client_zb_op_t op = {0};
    device_record_t *dev = find_device_by_id(device_id);

    if (!dev) {
        return CLIENT_ACTION_DEVICE_NOT_FOUND;
    }
    if (!dm_has_in_cluster(dev, cluster_id, &endpoint)) {
        return CLIENT_ACTION_UNSUPPORTED;
    }

    op.type = CLIENT_ZB_OP_READ_ATTR;
    op.dev_idx = (uint8_t)dm_index_of(dev);
    op.generation = dm_slot_generation(op.dev_idx);
    op.endpoint = endpoint;
    op.cluster_id = cluster_id;
    op.attr_id = attr_id;

    return schedule_zb_op(&op, delay_ms) ? CLIENT_ACTION_OK : CLIENT_ACTION_BUSY;
}

client_action_result_t client_actions_read_named_attribute(const char *device_id,
                                                           const char *name,
                                                           uint32_t delay_ms)
{
    const attr_name_map_t *map = find_attr_name_map(name);
    if (!map) {
        return CLIENT_ACTION_UNSUPPORTED;
    }
    return client_actions_read_attribute(device_id, map->cluster_id,
                                         map->attr_id, delay_ms);
}

client_action_result_t client_actions_set_permit_join(uint8_t duration_s)
{
    button_handler_set_permit_join_duration(duration_s);
    return CLIENT_ACTION_OK;
}

client_action_result_t client_actions_interview_device(const char *device_id)
{
    device_record_t *dev = find_device_by_id(device_id);
    if (!dev) {
        return CLIENT_ACTION_DEVICE_NOT_FOUND;
    }

    dev->state = DEV_STATE_NEW;
    return di_enqueue_async(dev) ? CLIENT_ACTION_OK : CLIENT_ACTION_BUSY;
}

client_action_result_t client_actions_configure_device(const char *device_id)
{
    device_record_t *dev = find_device_by_id(device_id);
    if (!dev) {
        return CLIENT_ACTION_DEVICE_NOT_FOUND;
    }
    if (dev->state < DEV_STATE_INTERVIEWED) {
        return CLIENT_ACTION_UNSUPPORTED;
    }

    rc_configure_device_async(dev);
    return CLIENT_ACTION_OK;
}

client_action_result_t client_actions_rename_device(const char *from_id,
                                                    const char *new_name)
{
    char cleaned_name[FRIENDLY_NAME_LEN];
    device_record_t *dev;
    device_record_t *existing;
    int slot_idx;

    trim_copy(new_name, cleaned_name, sizeof(cleaned_name));
    if (!from_id || !from_id[0] || cleaned_name[0] == '\0') {
        return CLIENT_ACTION_INVALID_ARG;
    }

    dev = find_device_by_id(from_id);
    if (!dev) {
        return CLIENT_ACTION_DEVICE_NOT_FOUND;
    }

    existing = dm_find_by_friendly_name(cleaned_name);
    if (existing && existing != dev) {
        return CLIENT_ACTION_INVALID_ARG;
    }

    dm_set_friendly_name(dev, cleaned_name);

    slot_idx = dm_index_of(dev);
    if (slot_idx >= 0) {
        nvs_cache_save_device((uint8_t)slot_idx);
    }

    return CLIENT_ACTION_OK;
}
