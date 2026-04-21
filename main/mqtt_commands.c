#include "mqtt_commands.h"

#include "mqtt_bridge.h"
#include "mqtt_manager.h"
#include "button_handler.h"
#include "device_interview.h"
#include "device_manager.h"
#include "nvs_cache.h"
#include "report_config.h"
#include "utils.h"

#include <cJSON.h>
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BRIDGE_REQUEST_PREFIX  MQTT_BASE_TOPIC "/bridge/request/"
#define BRIDGE_RESPONSE_PREFIX MQTT_BASE_TOPIC "/bridge/response/"
#define ENTITY_TOPIC_PREFIX    MQTT_BASE_TOPIC "/"
#define MQTT_ZB_OP_MAX         16

typedef enum {
    MQTT_ZB_OP_NONE = 0,
    MQTT_ZB_OP_SET_STATE,
    MQTT_ZB_OP_SET_BRIGHTNESS,
    MQTT_ZB_OP_READ_ATTR,
} mqtt_zb_op_type_t;

typedef struct {
    bool in_use;
    mqtt_zb_op_type_t type;
    uint8_t dev_idx;
    uint16_t generation;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t on_off_cmd_id;
    uint8_t level;
} mqtt_zb_op_t;

static mqtt_zb_op_t s_zb_ops[MQTT_ZB_OP_MAX];

typedef struct {
    const char *name;
    uint16_t cluster_id;
    uint16_t attr_id;
} get_attr_map_t;

static const get_attr_map_t k_get_attr_map[] = {
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

static void mqtt_zb_op_alarm(uint8_t slot_idx);

static bool schedule_zb_op(const mqtt_zb_op_t *op, uint32_t delay_ms)
{
    int idx;

    if (!op) {
        return false;
    }

    for (idx = 0; idx < MQTT_ZB_OP_MAX; idx++) {
        if (!s_zb_ops[idx].in_use) {
            s_zb_ops[idx] = *op;
            s_zb_ops[idx].in_use = true;
            esp_zb_scheduler_alarm(mqtt_zb_op_alarm, (uint8_t)idx, delay_ms);
            return true;
        }
    }

    return false;
}

static void clear_zb_op(uint8_t slot_idx)
{
    if (slot_idx >= MQTT_ZB_OP_MAX) {
        return;
    }
    memset(&s_zb_ops[slot_idx], 0, sizeof(s_zb_ops[slot_idx]));
}

static void mqtt_zb_op_alarm(uint8_t slot_idx)
{
    mqtt_zb_op_t *op;
    device_record_t *dev;

    if (slot_idx >= MQTT_ZB_OP_MAX) {
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
        case MQTT_ZB_OP_SET_STATE:
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
            ZB_LOG("MQTT SET state -> %s ep=%u cmd=0x%02X",
                   dm_display_name(dev), op->endpoint, op->on_off_cmd_id);
            esp_zb_zcl_on_off_cmd_req(&cmd);
            break;
        }

        case MQTT_ZB_OP_SET_BRIGHTNESS:
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
            ZB_LOG("MQTT SET brightness -> %s ep=%u level=%u",
                   dm_display_name(dev), op->endpoint, op->level);
            esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&cmd);
            break;
        }

        case MQTT_ZB_OP_READ_ATTR:
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
            ZB_LOG("MQTT GET -> %s ep=%u cluster=%s attr=%s",
                   dm_display_name(dev), op->endpoint,
                   utils_z2m_cluster_name(op->cluster_id),
                   utils_z2m_attribute_name(op->cluster_id, op->attr_id));
            esp_zb_zcl_read_attr_cmd_req(&cmd);
            break;
        }

        default:
            break;
    }

    clear_zb_op(slot_idx);
}

static bool schedule_read_attr_for_device(device_record_t *dev,
                                          uint16_t cluster_id,
                                          uint16_t attr_id,
                                          uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    mqtt_zb_op_t op = {0};

    if (!dev || !dm_has_in_cluster(dev, cluster_id, &endpoint)) {
        return false;
    }

    op.type = MQTT_ZB_OP_READ_ATTR;
    op.dev_idx = (uint8_t)dm_index_of(dev);
    op.generation = dm_slot_generation(op.dev_idx);
    op.endpoint = endpoint;
    op.cluster_id = cluster_id;
    op.attr_id = attr_id;
    return schedule_zb_op(&op, delay_ms);
}

static bool schedule_set_state_for_device(device_record_t *dev, const char *state,
                                          uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    mqtt_zb_op_t op = {0};

    if (!dev || !state || !dm_has_in_cluster(dev, 0x0006, &endpoint)) {
        return false;
    }

    op.type = MQTT_ZB_OP_SET_STATE;
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
        return false;
    }

    return schedule_zb_op(&op, delay_ms);
}

static bool schedule_set_brightness_for_device(device_record_t *dev, int brightness,
                                               uint32_t delay_ms)
{
    uint8_t endpoint = 0;
    mqtt_zb_op_t op = {0};

    if (!dev || brightness < 0 || brightness > 254 ||
        !dm_has_in_cluster(dev, 0x0008, &endpoint)) {
        return false;
    }

    op.type = MQTT_ZB_OP_SET_BRIGHTNESS;
    op.dev_idx = (uint8_t)dm_index_of(dev);
    op.generation = dm_slot_generation(op.dev_idx);
    op.endpoint = endpoint;
    op.level = (uint8_t)brightness;
    return schedule_zb_op(&op, delay_ms);
}

static const get_attr_map_t *find_get_attr_map(const char *name)
{
    size_t i;

    if (!name) {
        return NULL;
    }

    for (i = 0; i < sizeof(k_get_attr_map) / sizeof(k_get_attr_map[0]); i++) {
        if (strcmp(k_get_attr_map[i].name, name) == 0) {
            return &k_get_attr_map[i];
        }
    }

    return NULL;
}

static bool split_entity_topic(const char *topic, char *device_id,
                               size_t device_id_len, const char **verb_out,
                               const char **attr_out)
{
    const char *rel;
    const char *suffix = NULL;
    size_t prefix_len = strlen(ENTITY_TOPIC_PREFIX);
    size_t rel_len;
    size_t i;
    size_t name_len;

    if (!topic || strncmp(topic, ENTITY_TOPIC_PREFIX, prefix_len) != 0) {
        return false;
    }

    rel = topic + prefix_len;
    rel_len = strlen(rel);
    if (rel_len == 0) {
        return false;
    }

    if (rel_len > 4 && strcmp(rel + rel_len - 4, "/set") == 0) {
        suffix = rel + rel_len - 4;
        *verb_out = "set";
        *attr_out = NULL;
    } else if (rel_len > 4 && strcmp(rel + rel_len - 4, "/get") == 0) {
        suffix = rel + rel_len - 4;
        *verb_out = "get";
        *attr_out = NULL;
    } else {
        for (i = rel_len; i > 0; i--) {
            const char *p = rel + i - 1;
            if (strncmp(p, "/set/", 5) == 0) {
                suffix = p;
                *verb_out = "set";
                *attr_out = p + 5;
                break;
            }
            if (strncmp(p, "/get/", 5) == 0) {
                suffix = p;
                *verb_out = "get";
                *attr_out = p + 5;
                break;
            }
        }
    }

    if (!suffix) {
        return false;
    }

    name_len = (size_t)(suffix - rel);
    if (name_len == 0 || name_len >= device_id_len) {
        return false;
    }

    memcpy(device_id, rel, name_len);
    device_id[name_len] = '\0';
    return true;
}

static void publish_response(const char *request_key, cJSON *root)
{
    char topic[MQTT_MAX_TOPIC_LEN];
    char *payload;

    if (!request_key || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return;
    }

    snprintf(topic, sizeof(topic), BRIDGE_RESPONSE_PREFIX "%s", request_key);
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return;
    }

    mqtt_manager_publish(topic, payload, 0, false);
    cJSON_free(payload);
}

static void add_transaction(cJSON *response, const cJSON *request_root)
{
    cJSON *transaction;

    if (!response || !request_root || !cJSON_IsObject(request_root)) {
        return;
    }

    transaction = cJSON_GetObjectItemCaseSensitive((cJSON *)request_root,
                                                   "transaction");
    if (transaction) {
        cJSON_AddItemToObject(response, "transaction",
                              cJSON_Duplicate(transaction, true));
    }
}

static void publish_ok_response(const char *request_key, cJSON *data,
                                const cJSON *request_root)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        if (data) {
            cJSON_Delete(data);
        }
        return;
    }

    cJSON_AddItemToObject(response, "data", data ? data : cJSON_CreateObject());
    cJSON_AddStringToObject(response, "status", "ok");
    add_transaction(response, request_root);
    publish_response(request_key, response);
}

static void publish_error_response(const char *request_key, const char *error,
                                   const cJSON *request_root)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return;
    }

    cJSON_AddItemToObject(response, "data", cJSON_CreateObject());
    cJSON_AddStringToObject(response, "error",
                            error ? error : "Unknown error");
    cJSON_AddStringToObject(response, "status", "error");
    add_transaction(response, request_root);
    publish_response(request_key, response);
}

static cJSON *parse_request_json(const char *payload)
{
    if (!payload || payload[0] == '\0') {
        return NULL;
    }
    return cJSON_Parse(payload);
}

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

static bool extract_device_id(const char *payload, cJSON **root_out,
                              char *id_buf, size_t id_buf_len)
{
    cJSON *root = parse_request_json(payload);
    cJSON *id_item;

    if (root) {
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return false;
        }

        id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(id_item) || !id_item->valuestring ||
            id_item->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return false;
        }

        trim_copy(id_item->valuestring, id_buf, id_buf_len);
        if (root_out) {
            *root_out = root;
        } else {
            cJSON_Delete(root);
        }
        return id_buf[0] != '\0';
    }

    trim_copy(payload, id_buf, id_buf_len);
    if (id_buf[0] == '\0') {
        return false;
    }
    if (root_out) {
        *root_out = NULL;
    }
    return true;
}

static void handle_health_check(const char *request_key, const char *payload)
{
    cJSON *data;

    if (payload && payload[0] != '\0') {
        publish_error_response(request_key, "Payload must be empty", NULL);
        return;
    }

    data = cJSON_CreateObject();
    if (!data) {
        return;
    }
    cJSON_AddBoolToObject(data, "healthy", true);
    publish_ok_response(request_key, data, NULL);
}

static void handle_permit_join(const char *request_key, const char *payload)
{
    cJSON *root = parse_request_json(payload);
    cJSON *time_item;
    cJSON *value_item;
    int time_s = -1;
    cJSON *data;

    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        publish_error_response(request_key, "Invalid payload", NULL);
        return;
    }

    time_item = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (cJSON_IsNumber(time_item)) {
        time_s = time_item->valueint;
    }

    value_item = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (time_s < 0 && cJSON_IsBool(value_item)) {
        time_s = cJSON_IsTrue(value_item) ? 254 : 0;
    }

    if (time_s < 0 || time_s > 254) {
        publish_error_response(request_key, "Invalid payload", root);
        cJSON_Delete(root);
        return;
    }

    button_handler_set_permit_join_duration((uint8_t)time_s);

    data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(data, "time", time_s);
    publish_ok_response(request_key, data, root);
    cJSON_Delete(root);
}

static void handle_device_interview(const char *request_key, const char *payload)
{
    char id_buf[64];
    cJSON *root = NULL;
    device_record_t *dev;
    cJSON *data;

    if (!extract_device_id(payload, &root, id_buf, sizeof(id_buf))) {
        publish_error_response(request_key, "Invalid payload", NULL);
        return;
    }

    dev = find_device_by_id(id_buf);
    if (!dev) {
        publish_error_response(request_key, "Device not found", root);
        cJSON_Delete(root);
        return;
    }

    dev->state = DEV_STATE_NEW;
    di_enqueue(dev);

    data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "id", id_buf);
        publish_ok_response(request_key, data, root);
    }
    cJSON_Delete(root);
}

static void handle_device_configure(const char *request_key, const char *payload)
{
    char id_buf[64];
    cJSON *root = NULL;
    device_record_t *dev;
    cJSON *data;

    if (!extract_device_id(payload, &root, id_buf, sizeof(id_buf))) {
        publish_error_response(request_key, "Invalid payload", NULL);
        return;
    }

    dev = find_device_by_id(id_buf);
    if (!dev) {
        publish_error_response(request_key, "Device not found", root);
        cJSON_Delete(root);
        return;
    }

    if (dev->state < DEV_STATE_INTERVIEWED) {
        publish_error_response(request_key, "Device is not interviewed", root);
        cJSON_Delete(root);
        return;
    }

    rc_configure_device_async(dev);

    data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "id", id_buf);
        publish_ok_response(request_key, data, root);
    }
    cJSON_Delete(root);
}

static bool name_has_invalid_mqtt_wildcard(const char *name)
{
    return name && (strchr(name, '+') || strchr(name, '#'));
}

static void handle_device_rename(const char *request_key, const char *payload)
{
    cJSON *root = parse_request_json(payload);
    cJSON *from_item;
    cJSON *to_item;
    cJSON *last_item;
    cJSON *data;
    device_record_t *dev;
    device_record_t *existing;
    int slot_idx;
    char old_topic_name[64];
    char new_name[FRIENDLY_NAME_LEN];
    char from_name[64];

    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        publish_error_response(request_key, "Invalid payload", NULL);
        return;
    }

    to_item = cJSON_GetObjectItemCaseSensitive(root, "to");
    if (!cJSON_IsString(to_item) || !to_item->valuestring) {
        publish_error_response(request_key, "Invalid payload", root);
        cJSON_Delete(root);
        return;
    }

    trim_copy(to_item->valuestring, new_name, sizeof(new_name));
    if (new_name[0] == '\0' || name_has_invalid_mqtt_wildcard(new_name)) {
        publish_error_response(request_key, "Invalid target name", root);
        cJSON_Delete(root);
        return;
    }

    from_item = cJSON_GetObjectItemCaseSensitive(root, "from");
    last_item = cJSON_GetObjectItemCaseSensitive(root, "last");
    if (!cJSON_IsString(from_item) || !from_item->valuestring) {
        if (cJSON_IsTrue(last_item)) {
            publish_error_response(request_key, "last=true is not supported", root);
        } else {
            publish_error_response(request_key, "Invalid payload", root);
        }
        cJSON_Delete(root);
        return;
    }

    trim_copy(from_item->valuestring, from_name, sizeof(from_name));
    dev = find_device_by_id(from_name);
    if (!dev) {
        publish_error_response(request_key, "Device not found", root);
        cJSON_Delete(root);
        return;
    }

    existing = dm_find_by_friendly_name(new_name);
    if (existing && existing != dev) {
        publish_error_response(request_key, "friendly_name already in use", root);
        cJSON_Delete(root);
        return;
    }

    snprintf(old_topic_name, sizeof(old_topic_name), "%s", dm_display_name(dev));
    dm_set_friendly_name(dev, new_name);

    slot_idx = dm_index_of(dev);
    if (slot_idx >= 0) {
        nvs_cache_save_device((uint8_t)slot_idx);
    }

    mqtt_bridge_republish_device_after_rename(dev->ieee_addr,
                                              old_topic_name,
                                              dm_display_name(dev));

    data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "from", from_name);
        cJSON_AddStringToObject(data, "to", dm_display_name(dev));
        cJSON_AddBoolToObject(data, "homeassistant_rename", false);
        publish_ok_response(request_key, data, root);
    }
    cJSON_Delete(root);
}

static void handle_entity_set_json(device_record_t *dev, const char *payload)
{
    cJSON *root = parse_request_json(payload);
    cJSON *state_item;
    cJSON *brightness_item;
    bool has_state;
    bool has_brightness;
    uint32_t delay_ms = 0;

    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        ZB_LOG("MQTT SET invalid JSON payload");
        return;
    }

    state_item = cJSON_GetObjectItemCaseSensitive(root, "state");
    brightness_item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    has_state = cJSON_IsString(state_item) && state_item->valuestring;
    has_brightness = cJSON_IsNumber(brightness_item);

    if (has_state && has_brightness &&
        strcmp(state_item->valuestring, "OFF") == 0) {
        ZB_LOG("MQTT SET invalid payload: state=OFF with brightness");
        cJSON_Delete(root);
        return;
    }

    if (has_state) {
        if (!schedule_set_state_for_device(dev, state_item->valuestring, delay_ms)) {
            ZB_LOG("MQTT SET unsupported state for %s", dm_display_name(dev));
            cJSON_Delete(root);
            return;
        }
        delay_ms += 50;
    }

    if (has_brightness) {
        if (!schedule_set_brightness_for_device(dev, brightness_item->valueint, delay_ms)) {
            ZB_LOG("MQTT SET unsupported brightness for %s", dm_display_name(dev));
            cJSON_Delete(root);
            return;
        }
        delay_ms += 50;
    }

    if (!has_state && !has_brightness) {
        ZB_LOG("MQTT SET no supported keys for %s", dm_display_name(dev));
    }

    cJSON_Delete(root);
}

static void handle_entity_set_attr(device_record_t *dev, const char *attr,
                                   const char *payload)
{
    char value[32];

    trim_copy(payload, value, sizeof(value));
    if (!attr || value[0] == '\0') {
        ZB_LOG("MQTT SET attr invalid payload");
        return;
    }

    if (strcmp(attr, "state") == 0) {
        if (!schedule_set_state_for_device(dev, value, 0)) {
            ZB_LOG("MQTT SET unsupported state for %s", dm_display_name(dev));
        }
        return;
    }

    if (strcmp(attr, "brightness") == 0) {
        int brightness = atoi(value);
        if (!schedule_set_brightness_for_device(dev, brightness, 0)) {
            ZB_LOG("MQTT SET unsupported brightness for %s", dm_display_name(dev));
        }
        return;
    }

    ZB_LOG("MQTT SET unsupported attr=%s", attr);
}

static void handle_entity_set(const char *device_id, const char *attr,
                              const char *payload)
{
    device_record_t *dev = find_device_by_id(device_id);

    if (!dev) {
        ZB_LOG("MQTT SET device not found id=%s", device_id ? device_id : "");
        return;
    }

    if (attr) {
        handle_entity_set_attr(dev, attr, payload);
    } else {
        handle_entity_set_json(dev, payload);
    }
}

static void schedule_entity_get_key(device_record_t *dev, const char *key,
                                    uint32_t *delay_ms)
{
    const get_attr_map_t *map = find_get_attr_map(key);

    if (!map) {
        ZB_LOG("MQTT GET unsupported key=%s", key ? key : "");
        return;
    }

    if (!schedule_read_attr_for_device(dev, map->cluster_id, map->attr_id,
                                       delay_ms ? *delay_ms : 0)) {
        ZB_LOG("MQTT GET key=%s not supported by %s",
               key, dm_display_name(dev));
        return;
    }

    if (delay_ms) {
        *delay_ms += 25;
    }
}

static void handle_entity_get_json(device_record_t *dev, const char *payload)
{
    cJSON *root = parse_request_json(payload);
    cJSON *child;
    uint32_t delay_ms = 0;

    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        ZB_LOG("MQTT GET invalid JSON payload");
        return;
    }

    child = root->child;
    while (child) {
        if (child->string) {
            schedule_entity_get_key(dev, child->string, &delay_ms);
        }
        child = child->next;
    }

    cJSON_Delete(root);
}

static void handle_entity_get(const char *device_id, const char *attr,
                              const char *payload)
{
    device_record_t *dev = find_device_by_id(device_id);
    uint32_t delay_ms = 0;

    (void)payload;

    if (!dev) {
        ZB_LOG("MQTT GET device not found id=%s", device_id ? device_id : "");
        return;
    }

    if (attr) {
        schedule_entity_get_key(dev, attr, &delay_ms);
    } else {
        handle_entity_get_json(dev, payload);
    }
}

void mqtt_commands_on_message(const char *topic, const char *payload)
{
    const char *request_key;
    char device_id[FRIENDLY_NAME_LEN];
    const char *verb = NULL;
    const char *attr = NULL;

    if (!topic) {
        return;
    }

    if (split_entity_topic(topic, device_id, sizeof(device_id), &verb, &attr)) {
        if (strcmp(verb, "set") == 0) {
            handle_entity_set(device_id, attr, payload);
        } else if (strcmp(verb, "get") == 0) {
            handle_entity_get(device_id, attr, payload);
        }
        return;
    }

    if (strncmp(topic, BRIDGE_REQUEST_PREFIX,
                strlen(BRIDGE_REQUEST_PREFIX)) != 0) {
        ZB_LOG("MQTT RX unsupported topic=%s", topic);
        return;
    }

    request_key = topic + strlen(BRIDGE_REQUEST_PREFIX);
    if (strcmp(request_key, "permit_join") == 0) {
        handle_permit_join(request_key, payload);
    } else if (strcmp(request_key, "health_check") == 0) {
        handle_health_check(request_key, payload);
    } else if (strcmp(request_key, "device/rename") == 0) {
        handle_device_rename(request_key, payload);
    } else if (strcmp(request_key, "device/interview") == 0) {
        handle_device_interview(request_key, payload);
    } else if (strcmp(request_key, "device/configure") == 0) {
        handle_device_configure(request_key, payload);
    } else {
        publish_error_response(request_key, "Unsupported request", NULL);
    }
}
