/*
 * ENC28J60 <-> ESP32-C5-DevKitC cableado para el coordinador:
 *
 *   SI (MOSI)  -> GPIO4
 *   SO (MISO)  -> GPIO5
 *   SCK        -> GPIO6
 *   CS         -> GPIO23
 *   INT        -> GPIO24
 *   RESET      -> GPIO25
 *   VCC        -> 3V3
 *   GND        -> GND
 *
 * WOL y CLOCKOUT: no conectar.
 *
 * Notas:
 * - Este firmware usa solo Ethernet por SPI; WiFi queda desactivado en sdkconfig.
 * - El RESET del modulo se conmuta manualmente desde GPIO25 antes de instalar esp_eth.
 * - ENC28J60 no trae MAC valida de fabrica; se deriva una MAC local desde la eFuse del ESP32-C5.
 * - Se usa un reloj SPI conservador (10 MHz) y se ajusta el CS hold time segun el driver oficial.
 * - Al arrancar mDNS pueden verse logs de "add mac filter not supported" desde esp_eth/esp_netif.
 *   En este driver ENC28J60 el filtro multicast fino no se implementa, pero el chip ya se configura
 *   para aceptar trafico multicast, asi que mDNS puede seguir funcionando igualmente.
 */

#include "mqtt_bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_enc28j60.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "timebase.h"
#include "zb_coordinator.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

#include "device_table.h"

namespace mqtt_bridge {

static const char *TAG = "mqtt_bridge";
static constexpr spi_host_device_t kSpiHost = SPI2_HOST;
static constexpr int kSpiClockMHz = 10;
static constexpr gpio_num_t kSpiMosiGpio = GPIO_NUM_4;
static constexpr gpio_num_t kSpiMisoGpio = GPIO_NUM_5;
static constexpr gpio_num_t kSpiSclkGpio = GPIO_NUM_6;
static constexpr gpio_num_t kSpiCsGpio = GPIO_NUM_23;
static constexpr gpio_num_t kIntGpio = GPIO_NUM_24;
static constexpr gpio_num_t kResetGpio = GPIO_NUM_25;
static constexpr uint8_t kCoordinatorEndpoint = 1;
static constexpr size_t kInboundQueueLen = 8;
static constexpr size_t kZigbeeEventQueueLen = 24;
static constexpr size_t kTopicMax = 192;
static constexpr size_t kPayloadMax = 256;
static constexpr size_t kPendingEventMax = 16;
static constexpr TickType_t kHealthPublishPeriodTicks = pdMS_TO_TICKS(30000);
static constexpr TickType_t kDefaultPermitJoinTicks = pdMS_TO_TICKS(180000);
static constexpr TickType_t kRestartDelayTicks = pdMS_TO_TICKS(250);
static constexpr TickType_t kBridgePollPeriodTicks = pdMS_TO_TICKS(10);
static constexpr uint32_t kBridgeTaskStackSize = 8192;
static constexpr UBaseType_t kBridgeTaskPriority = 6;
static constexpr const char *kOutputMode = "json";
static constexpr bool kAvailabilityEnabled = true;
static constexpr bool kIncludeDeviceInformation = false;

typedef struct {
    char topic[kTopicMax];
    char payload[kPayloadMax];
} mqtt_inbound_msg_t;

struct entity_request_t {
    std::string name;
    std::string action;
    std::string attribute;
    uint8_t endpoint = 0;
    bool has_endpoint = false;
};

static const char *enc28j60_rev_to_str(eth_enc28j60_rev_t rev)
{
    switch (rev) {
    case ENC28J60_REV_B1: return "B1";
    case ENC28J60_REV_B4: return "B4";
    case ENC28J60_REV_B5: return "B5";
    case ENC28J60_REV_B7: return "B7";
    default: return "unknown";
    }
}

static std::string slugify_copy(const char *text)
{
    std::string out;
    bool last_sep = false;
    if (text != nullptr) {
        for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
            const unsigned char ch = *p;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
                out.push_back((char)ch);
                last_sep = false;
            } else if (ch >= 'A' && ch <= 'Z') {
                out.push_back((char)(ch - 'A' + 'a'));
                last_sep = false;
            } else if (!last_sep && !out.empty()) {
                out.push_back('_');
                last_sep = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

static std::string device_friendly_name(const device_record_t &dev)
{
    std::string base = slugify_copy(dev.model);
    if (base.empty()) {
        base = slugify_copy(dev.norm_name);
    }
    if (base.empty()) {
        base = slugify_copy(dev.manufacturer);
    }
    if (base.empty()) {
        base = "device";
    }
    char suffix[10];
    snprintf(suffix, sizeof(suffix), "%08llx", (unsigned long long)(dev.ieee & 0xFFFFFFFFULL));
    base.push_back('_');
    base.append(suffix);
    return base;
}

static std::string ieee_text(uint64_t ieee)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)ieee);
    return std::string(buf);
}

static std::string short_text(uint16_t short_addr)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%04X", short_addr);
    return std::string(buf);
}

static bool parse_uint8_token(const std::string &value, uint8_t *out)
{
    if (out == nullptr || value.empty()) {
        return false;
    }
    char *end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 255) {
        return false;
    }
    *out = (uint8_t)parsed;
    return true;
}

static bool parse_bool_token(const char *value, bool *out)
{
    if (value == nullptr || out == nullptr) {
        return false;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
        strcmp(value, "on") == 0 || strcmp(value, "ON") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 ||
        strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static std::string trim_copy(const char *text)
{
    if (text == nullptr) {
        return std::string();
    }
    const char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }
    const char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    return std::string(start, (size_t)(end - start));
}

static bool parse_state_payload(const char *payload, bool *out_on)
{
    const std::string text = trim_copy(payload);
    if (parse_bool_token(text.c_str(), out_on)) {
        return true;
    }
    cJSON *root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        return false;
    }
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const bool ok = cJSON_IsString(state) && state->valuestring != nullptr && parse_bool_token(state->valuestring, out_on);
    cJSON_Delete(root);
    return ok;
}

static bool parse_networkmap_format(const char *payload, std::string *out_type)
{
    if (out_type == nullptr) {
        return false;
    }
    *out_type = "raw";
    const std::string text = trim_copy(payload);
    if (text.empty()) {
        return true;
    }
    if (text == "raw" || text == "graphviz" || text == "plantuml") {
        *out_type = text;
        return true;
    }
    cJSON *root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == nullptr) {
        type = cJSON_GetObjectItemCaseSensitive(root, "value");
    }
    const bool ok = cJSON_IsString(type) && type->valuestring != nullptr &&
                    (strcmp(type->valuestring, "raw") == 0 || strcmp(type->valuestring, "graphviz") == 0 ||
                     strcmp(type->valuestring, "plantuml") == 0);
    if (ok) {
        *out_type = type->valuestring;
    }
    cJSON_Delete(root);
    return ok;
}

static std::vector<std::string> split_topic(const std::string &text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t slash = text.find('/', start);
        if (slash == std::string::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, slash - start));
        start = slash + 1U;
    }
    return parts;
}

static bool parse_entity_request(const std::string &rel, entity_request_t *out)
{
    if (out == nullptr) {
        return false;
    }
    const std::vector<std::string> parts = split_topic(rel);
    if (parts.empty() || parts[0].empty() || parts[0] == "bridge") {
        return false;
    }
    entity_request_t req = {};
    req.name = parts[0];
    if (parts.size() == 2U && (parts[1] == "set" || parts[1] == "get")) {
        req.action = parts[1];
        *out = req;
        return true;
    }
    if (parts.size() == 3U && (parts[1] == "set" || parts[1] == "get")) {
        req.action = parts[1];
        req.attribute = parts[2];
        *out = req;
        return true;
    }
    if (parts.size() == 3U && parse_uint8_token(parts[1], &req.endpoint) && (parts[2] == "set" || parts[2] == "get")) {
        req.has_endpoint = true;
        req.action = parts[2];
        *out = req;
        return true;
    }
    if (parts.size() == 4U && parse_uint8_token(parts[1], &req.endpoint) && (parts[2] == "set" || parts[2] == "get")) {
        req.has_endpoint = true;
        req.action = parts[2];
        req.attribute = parts[3];
        *out = req;
        return true;
    }
    return false;
}

class Bridge {
public:
    esp_err_t init();
    void poll();
    esp_err_t notify_zigbee_event(const char *event_name);

private:
    esp_err_t ensure_network_stack();
    esp_err_t register_event_handlers();
    esp_err_t hardware_reset();
    esp_err_t init_spi_bus();
    esp_err_t init_ethernet();
    esp_err_t attach_netif();
    esp_err_t start_mdns();
    esp_err_t start_mqtt();
    esp_err_t create_inbound_queue();
    esp_err_t create_event_queue();
    esp_err_t create_worker_task();
    void wake_worker();

    void process_inbound();
    void process_events();
    void handle_message(const mqtt_inbound_msg_t &msg);
    void handle_bridge_request(const std::string &request, const char *payload);
    void handle_entity_request(const entity_request_t &req, const char *payload);
    void queue_full_publish();

    int publish_text(const std::string &rel_topic, const char *payload, bool retain, int qos);
    esp_err_t publish_json(const std::string &rel_topic, cJSON *json, bool retain, int qos);
    esp_err_t publish_bridge_log(const char *level, const char *ns, const std::string &message);
    esp_err_t publish_bridge_state(const char *state);
    esp_err_t publish_bridge_info(bool retain);
    esp_err_t publish_bridge_devices(bool retain);
    esp_err_t publish_bridge_groups(bool retain);
    esp_err_t publish_bridge_definitions(bool retain);
    esp_err_t publish_bridge_health(bool retain);
    esp_err_t publish_bridge_event(const zb_coord_event_info_t *event_info);
    esp_err_t publish_all_device_states(bool retain);
    esp_err_t publish_device_state(const device_record_t &dev, bool retain, int endpoint_filter = -1);
    esp_err_t publish_selected_device_state(uint64_t ieee, uint16_t short_addr, bool retain);
    esp_err_t publish_single_attribute(const device_record_t &dev, const std::string &attribute, bool retain, int endpoint_filter = -1);
    esp_err_t publish_response(const char *suffix, cJSON *json);
    esp_err_t send_on_off_command(const device_record_t &dev, uint8_t endpoint, bool on);

    static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void on_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void bridge_task_entry(void *arg);

    bool initialized_ = false;
    bool handlers_registered_ = false;
    bool spi_bus_ready_ = false;
    bool mdns_started_ = false;
    bool mqtt_started_ = false;
    volatile bool mqtt_connected_ = false;
    bool permit_join_open_ = false;
    bool publish_info_pending_ = false;
    bool publish_devices_pending_ = false;
    bool publish_groups_pending_ = false;
    bool publish_definitions_pending_ = false;
    bool publish_states_pending_ = false;
    bool publish_online_pending_ = false;
    bool restart_pending_ = false;
    TickType_t next_health_tick_ = 0;
    TickType_t permit_join_deadline_ = 0;
    TickType_t restart_deadline_ = 0;
    esp_eth_handle_t eth_handle_ = nullptr;
    esp_eth_mac_t *mac_ = nullptr;
    esp_eth_phy_t *phy_ = nullptr;
    esp_netif_t *netif_ = nullptr;
    esp_eth_netif_glue_handle_t eth_glue_ = nullptr;
    esp_mqtt_client_handle_t mqtt_client_ = nullptr;
    QueueHandle_t inbound_queue_ = nullptr;
    QueueHandle_t zigbee_event_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    esp_event_handler_instance_t eth_event_instance_ = nullptr;
    esp_event_handler_instance_t got_ip_instance_ = nullptr;
    esp_event_handler_instance_t lost_ip_instance_ = nullptr;
};

static Bridge s_bridge;

static bool endpoint_has_cluster(const device_endpoint_record_t &ep, uint16_t cluster_id)
{
    for (size_t i = 0; i < ep.input_clusters_len; ++i) {
        if (ep.input_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static bool find_on_off_endpoint(const device_record_t &dev, uint8_t *endpoint_out)
{
    if (endpoint_out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t &ep = dev.endpoints[i];
        if (ep.used && endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)) {
            *endpoint_out = ep.endpoint_id;
            return true;
        }
    }
    return false;
}

static bool load_device_by_name(const std::string &name, device_record_t *out)
{
    if (out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, out)) {
            continue;
        }
        if (device_friendly_name(*out) == name) {
            return true;
        }
    }
    return false;
}

static bool load_device_by_short(uint16_t short_addr, device_record_t *out)
{
    if (out == nullptr || short_addr == 0U) {
        return false;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, out)) {
            continue;
        }
        if (out->short_addr == short_addr) {
            return true;
        }
    }
    return false;
}

static bool load_device_by_ieee(uint64_t ieee, device_record_t *out)
{
    if (out == nullptr || ieee == 0U) {
        return false;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, out)) {
            continue;
        }
        if (out->ieee == ieee) {
            return true;
        }
    }
    return false;
}

static void add_device_ref_json(cJSON *obj, const device_record_t *dev)
{
    if (obj == nullptr || dev == nullptr) {
        return;
    }
    cJSON_AddStringToObject(obj, "friendly_name", device_friendly_name(*dev).c_str());
    cJSON_AddStringToObject(obj, "ieee_address", ieee_text(dev->ieee).c_str());
    cJSON_AddStringToObject(obj, "manufacturer", dev->manufacturer);
    cJSON_AddStringToObject(obj, "model", dev->model);
    cJSON_AddStringToObject(obj, "norm_type", dev->norm_name);
}

static void add_event_ref_json(cJSON *obj, const zb_coord_event_info_t *info)
{
    if (obj == nullptr || info == nullptr || !info->has_device) {
        return;
    }
    if (info->ieee != 0U) {
        cJSON_AddStringToObject(obj, "ieee_address", ieee_text(info->ieee).c_str());
    }
}

static bool device_definition_supported(const device_record_t &dev)
{
    return dev.norm_type != DEVICE_NORM_UNKNOWN || dev.model[0] != '\0' || dev.manufacturer[0] != '\0';
}

static cJSON *build_device_definition_json(const device_record_t &dev)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "model", dev.model);
    cJSON_AddStringToObject(root, "vendor", dev.manufacturer);
    cJSON_AddStringToObject(root, "description", dev.norm_name);
    cJSON *exposes = cJSON_CreateArray();
    if (exposes == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    if (dev.norm_type == DEVICE_NORM_SWITCH) {
        cJSON_AddItemToArray(exposes, cJSON_CreateString("state"));
    }
    if (dev.norm_type == DEVICE_NORM_TEMP_HUMIDITY || dev.norm_type == DEVICE_NORM_TEMP) {
        cJSON_AddItemToArray(exposes, cJSON_CreateString("temperature"));
    }
    if (dev.norm_type == DEVICE_NORM_TEMP_HUMIDITY) {
        cJSON_AddItemToArray(exposes, cJSON_CreateString("humidity"));
        cJSON_AddItemToArray(exposes, cJSON_CreateString("battery"));
    }
    if (dev.norm_type == DEVICE_NORM_PRESENCE) {
        cJSON_AddItemToArray(exposes, cJSON_CreateString("occupancy"));
        cJSON_AddItemToArray(exposes, cJSON_CreateString("illuminance"));
    }
    cJSON_AddItemToObject(root, "exposes", exposes);
    return root;
}

static bool event_should_publish_bridge_event(const char *event_name)
{
    return event_name != nullptr &&
           (strcmp(event_name, "DEVICE_UPDATE") == 0 || strcmp(event_name, "DEVICE_ANNOUNCE") == 0 ||
            strcmp(event_name, "DEVICE_LEAVE") == 0 || strcmp(event_name, "INTERVIEW_START") == 0 ||
            strcmp(event_name, "INTERVIEW_SIMPLE_DESC") == 0 || strcmp(event_name, "DEVICE_AUTH_FAILED") == 0);
}

static bool event_should_refresh_device_state(const char *event_name)
{
    return event_name != nullptr &&
           (strcmp(event_name, "DEVICE_REPORT") == 0 || strcmp(event_name, "DEVICE_ANNOUNCE") == 0 ||
            strcmp(event_name, "DEVICE_UPDATE") == 0 || strcmp(event_name, "DEVICE_LEAVE") == 0 ||
            strcmp(event_name, "INTERVIEW_SIMPLE_DESC") == 0);
}

static cJSON *build_networkmap_array(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) {
        return nullptr;
    }
    device_record_t parent = {};
    device_record_t child = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &parent)) {
            continue;
        }
        if (!parent.occupied || !parent.in_network) {
            continue;
        }
        cJSON *node = cJSON_CreateObject();
        if (node == nullptr) {
            cJSON_Delete(arr);
            return nullptr;
        }
        add_device_ref_json(node, &parent);
        cJSON *children = cJSON_CreateArray();
        if (children == nullptr) {
            cJSON_Delete(node);
            cJSON_Delete(arr);
            return nullptr;
        }
        uint32_t child_count = 0;
        for (size_t j = 0; j < DEVICE_TABLE_MAX_DEVICES; ++j) {
            if (!device_table_copy_device_at(j, &child)) {
                continue;
            }
            if (!child.occupied || !child.in_network || child.parent_short != parent.short_addr) {
                continue;
            }
            cJSON *child_item = cJSON_CreateObject();
            if (child_item == nullptr) {
                cJSON_Delete(children);
                cJSON_Delete(node);
                cJSON_Delete(arr);
                return nullptr;
            }
            add_device_ref_json(child_item, &child);
            cJSON_AddItemToArray(children, child_item);
            child_count++;
        }
        cJSON_AddNumberToObject(node, "children_count", child_count);
        cJSON_AddItemToObject(node, "children", children);
        cJSON_AddItemToArray(arr, node);
    }
    return arr;
}

static std::string build_networkmap_graphviz(void)
{
    std::string out = "digraph zigbee {\n";
    device_record_t dev = {};
    device_record_t parent = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        if (!dev.occupied || !dev.in_network) {
            continue;
        }
        const std::string name = device_friendly_name(dev);
        out += "  \"";
        out += name;
        out += "\" [label=\"";
        out += name;
        out += "\\n";
        out += short_text(dev.short_addr);
        out += "\"];\n";
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        const device_record_t &child = dev;
        if (!child.occupied || !child.in_network) {
            continue;
        }
        if (!load_device_by_short(child.parent_short, &parent) || parent.short_addr == child.short_addr) {
            continue;
        }
        out += "  \"";
        out += device_friendly_name(parent);
        out += "\" -> \"";
        out += device_friendly_name(child);
        out += "\";\n";
    }
    out += "}\n";
    return out;
}

static std::string build_networkmap_plantuml(void)
{
    std::string out = "@startuml\n";
    out += "left to right direction\n";
    device_record_t dev = {};
    device_record_t parent = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        if (!dev.occupied || !dev.in_network) {
            continue;
        }
        char alias[24];
        snprintf(alias, sizeof(alias), "n%04x", (unsigned)dev.short_addr);
        out += "object \"";
        out += device_friendly_name(dev);
        out += "\\n";
        out += short_text(dev.short_addr);
        out += "\" as ";
        out += alias;
        out += "\n";
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        const device_record_t &child = dev;
        if (!child.occupied || !child.in_network) {
            continue;
        }
        if (!load_device_by_short(child.parent_short, &parent) || parent.short_addr == child.short_addr) {
            continue;
        }
        char parent_alias[24];
        char child_alias[24];
        snprintf(parent_alias, sizeof(parent_alias), "n%04x", (unsigned)parent.short_addr);
        snprintf(child_alias, sizeof(child_alias), "n%04x", (unsigned)child.short_addr);
        out += parent_alias;
        out += " --> ";
        out += child_alias;
        out += "\n";
    }
    out += "@enduml\n";
    return out;
}

static bool endpoint_matches_filter(const device_endpoint_record_t &ep, int endpoint_filter)
{
    return ep.used && (endpoint_filter < 0 || ep.endpoint_id == (uint8_t)endpoint_filter);
}

static std::string full_topic(const std::string &rel_topic)
{
    std::string topic(CONFIG_MQTT_BRIDGE_BASE_TOPIC);
    if (!rel_topic.empty()) {
        topic.push_back('/');
        topic.append(rel_topic);
    }
    return topic;
}

esp_err_t Bridge::ensure_network_stack()
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

esp_err_t Bridge::register_event_handlers()
{
    if (handlers_registered_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &Bridge::on_eth_event, this, &eth_event_instance_),
        TAG, "No se pudo registrar ETH_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &Bridge::on_ip_event, this, &got_ip_instance_),
        TAG, "No se pudo registrar ETH_GOT_IP");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &Bridge::on_ip_event, this, &lost_ip_instance_),
        TAG, "No se pudo registrar ETH_LOST_IP");
    handlers_registered_ = true;
    return ESP_OK;
}

esp_err_t Bridge::hardware_reset()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << kResetGpio);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "No se pudo configurar RESET ENC28J60");
    gpio_set_level(kResetGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kResetGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t Bridge::init_spi_bus()
{
    if (spi_bus_ready_) {
        return ESP_OK;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = kSpiMosiGpio;
    buscfg.miso_io_num = kSpiMisoGpio;
    buscfg.sclk_io_num = kSpiSclkGpio;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    err = spi_bus_initialize(kSpiHost, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    spi_bus_ready_ = true;
    return ESP_OK;
}

esp_err_t Bridge::init_ethernet()
{
    if (eth_handle_ != nullptr) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "Reset ENC28J60 fallido");
    ESP_RETURN_ON_ERROR(init_spi_bus(), TAG, "No se pudo inicializar SPI ENC28J60");
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = -1;
    phy_config.autonego_timeout_ms = 0;
    spi_device_interface_config_t spi_devcfg = {};
    spi_devcfg.mode = 0;
    spi_devcfg.clock_speed_hz = kSpiClockMHz * 1000 * 1000;
    spi_devcfg.spics_io_num = kSpiCsGpio;
    spi_devcfg.queue_size = 20;
    spi_devcfg.cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(kSpiClockMHz);
    eth_enc28j60_config_t enc_config = ETH_ENC28J60_DEFAULT_CONFIG(kSpiHost, &spi_devcfg);
    enc_config.int_gpio_num = kIntGpio;
    mac_ = esp_eth_mac_new_enc28j60(&enc_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac_ != nullptr, ESP_FAIL, TAG, "No se pudo crear MAC ENC28J60");
    phy_ = esp_eth_phy_new_enc28j60(&phy_config);
    ESP_RETURN_ON_FALSE(phy_ != nullptr, ESP_FAIL, TAG, "No se pudo crear PHY ENC28J60");
    ESP_LOGI(TAG, "[T+%07.3f] ENC28J60 detectado, revision %s", timebase_now_s(),
             enc28j60_rev_to_str(emac_enc28j60_get_chip_info(mac_)));
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac_, phy_);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle_), TAG, "Fallo instalando esp_eth");
    uint8_t base_mac[ETH_ADDR_LEN] = {};
    uint8_t local_mac[ETH_ADDR_LEN] = {};
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(base_mac), TAG, "No se pudo leer MAC base");
    esp_derive_local_mac(local_mac, base_mac);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle_, ETH_CMD_S_MAC_ADDR, local_mac), TAG, "No se pudo fijar MAC");
    return ESP_OK;
}

esp_err_t Bridge::attach_netif()
{
    if (netif_ != nullptr) {
        return ESP_OK;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_ = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(netif_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear esp_netif");
    eth_glue_ = esp_eth_new_netif_glue(eth_handle_);
    ESP_RETURN_ON_FALSE(eth_glue_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif_, eth_glue_), TAG, "No se pudo adjuntar netif");
    return ESP_OK;
}

esp_err_t Bridge::create_inbound_queue()
{
    if (inbound_queue_ != nullptr) {
        return ESP_OK;
    }
    inbound_queue_ = xQueueCreate(kInboundQueueLen, sizeof(mqtt_inbound_msg_t));
    ESP_RETURN_ON_FALSE(inbound_queue_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear cola MQTT");
    return ESP_OK;
}

esp_err_t Bridge::create_event_queue()
{
    if (zigbee_event_queue_ != nullptr) {
        return ESP_OK;
    }
    zigbee_event_queue_ = xQueueCreate(kZigbeeEventQueueLen, sizeof(zb_coord_event_info_t));
    ESP_RETURN_ON_FALSE(zigbee_event_queue_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear cola eventos Zigbee");
    return ESP_OK;
}

esp_err_t Bridge::create_worker_task()
{
    if (worker_task_ != nullptr) {
        return ESP_OK;
    }
    const BaseType_t ok = xTaskCreate(&Bridge::bridge_task_entry, "mqtt_bridge_task", kBridgeTaskStackSize, this, kBridgeTaskPriority, &worker_task_);
    ESP_RETURN_ON_FALSE(ok == pdPASS && worker_task_ != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo crear tarea mqtt_bridge");
    return ESP_OK;
}

void Bridge::wake_worker()
{
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
}

esp_err_t Bridge::start_mdns()
{
    if (mdns_started_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "No se pudo iniciar mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_MQTT_BRIDGE_MDNS_HOSTNAME), TAG, "No se pudo fijar hostname mDNS");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(CONFIG_MQTT_BRIDGE_MDNS_INSTANCE), TAG, "No se pudo fijar instancia mDNS");
    mdns_started_ = true;
    ESP_LOGI(TAG, "[T+%07.3f] mDNS activo como %s.local (%s)", timebase_now_s(),
             CONFIG_MQTT_BRIDGE_MDNS_HOSTNAME, CONFIG_MQTT_BRIDGE_MDNS_INSTANCE);
    return ESP_OK;
}

esp_err_t Bridge::start_mqtt()
{
    if (mqtt_started_) {
        return ESP_OK;
    }
    if (strlen(CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI) == 0) {
        ESP_LOGW(TAG, "[T+%07.3f] MQTT deshabilitado: broker URI vacio", timebase_now_s());
        return ESP_OK;
    }
    const std::string lwt_topic = full_topic("bridge/state");
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI;
    mqtt_cfg.credentials.client_id = CONFIG_MQTT_BRIDGE_MQTT_CLIENT_ID;
    mqtt_cfg.session.last_will.topic = lwt_topic.c_str();
    mqtt_cfg.session.last_will.msg = "{\"state\":\"offline\"}";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;
    mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(mqtt_client_ != nullptr, ESP_FAIL, TAG, "No se pudo crear cliente MQTT");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY, &Bridge::on_mqtt_event, this),
                        TAG, "No se pudo registrar handler MQTT");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(mqtt_client_), TAG, "No se pudo arrancar MQTT");
    mqtt_started_ = true;
    ESP_LOGI(TAG, "[T+%07.3f] MQTT arrancado contra %s", timebase_now_s(), CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI);
    return ESP_OK;
}

int Bridge::publish_text(const std::string &rel_topic, const char *payload, bool retain, int qos)
{
    if (mqtt_client_ == nullptr || !mqtt_connected_) {
        return -1;
    }
    return esp_mqtt_client_publish(mqtt_client_, full_topic(rel_topic).c_str(), payload, 0, qos, retain ? 1 : 0);
}

esp_err_t Bridge::publish_json(const std::string &rel_topic, cJSON *json, bool retain, int qos)
{
    if (json == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    ESP_RETURN_ON_FALSE(payload != nullptr, ESP_ERR_NO_MEM, TAG, "No se pudo serializar JSON MQTT");
    const int msg_id = publish_text(rel_topic, payload, retain, qos);
    free(payload);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "Fallo publicando MQTT");
    return ESP_OK;
}

esp_err_t Bridge::publish_bridge_log(const char *level, const char *ns, const std::string &message)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/logging");
    cJSON_AddStringToObject(root, "level", level != nullptr ? level : "info");
    cJSON_AddStringToObject(root, "namespace", ns != nullptr ? ns : TAG);
    cJSON_AddStringToObject(root, "message", message.c_str());
    return publish_json("bridge/logging", root, false, 0);
}

esp_err_t Bridge::publish_bridge_state(const char *state)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/state");
    cJSON_AddStringToObject(root, "state", state != nullptr ? state : "unknown");
    return publish_json("bridge/state", root, true, 1);
}

esp_err_t Bridge::publish_bridge_info(bool retain)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    zb_network_runtime_t runtime = {};
    device_table_network_summary_t summary = {};
    device_table_telemetry_t telemetry = {};
    (void)zb_coordinator_get_runtime_state(&runtime);
    device_table_get_network_summary(&summary);
    device_table_get_telemetry(&telemetry);

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/info");
    cJSON_AddStringToObject(root, "version", app_desc != nullptr ? app_desc->version : "unknown");
    cJSON_AddStringToObject(root, "os", esp_get_idf_version());
    cJSON *mqtt = cJSON_CreateObject();
    cJSON *coordinator = cJSON_CreateObject();
    cJSON *network = cJSON_CreateObject();
    cJSON *config = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(mqtt != nullptr && coordinator != nullptr && network != nullptr && config != nullptr,
                        ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/info subobjects");
    cJSON_AddStringToObject(mqtt, "base_topic", CONFIG_MQTT_BRIDGE_BASE_TOPIC);
    cJSON_AddStringToObject(mqtt, "broker", CONFIG_MQTT_BRIDGE_MQTT_BROKER_URI);
    cJSON_AddStringToObject(mqtt, "client_id", CONFIG_MQTT_BRIDGE_MQTT_CLIENT_ID);
    cJSON_AddStringToObject(mqtt, "state", mqtt_connected_ ? "online" : "offline");
    cJSON_AddStringToObject(coordinator, "short_address", short_text(runtime.short_addr).c_str());
    cJSON_AddStringToObject(coordinator, "mdns_hostname", CONFIG_MQTT_BRIDGE_MDNS_HOSTNAME);
    cJSON_AddBoolToObject(network, "online", runtime.has_network);
    cJSON_AddBoolToObject(network, "permit_join", permit_join_open_);
    cJSON_AddNumberToObject(network, "channel", runtime.channel);
    cJSON_AddStringToObject(network, "pan_id", short_text(runtime.pan_id).c_str());
    cJSON_AddNumberToObject(network, "nodes_total", summary.nodes_total);
    cJSON_AddStringToObject(config, "output", kOutputMode);
    cJSON_AddBoolToObject(config, "availability", kAvailabilityEnabled);
    cJSON_AddBoolToObject(config, "include_device_information", kIncludeDeviceInformation);
    cJSON_AddItemToObject(root, "mqtt", mqtt);
    cJSON_AddItemToObject(root, "coordinator", coordinator);
    cJSON_AddItemToObject(root, "network", network);
    cJSON_AddItemToObject(root, "config", config);
    cJSON_AddNumberToObject(root, "report_attr_ok", telemetry.report_attr_ok);
    cJSON_AddNumberToObject(root, "report_attr_unchanged", telemetry.report_attr_unchanged);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    return publish_json("bridge/info", root, retain, 1);
}

esp_err_t Bridge::publish_single_attribute(const device_record_t &dev, const std::string &attribute, bool retain, int endpoint_filter)
{
    const std::string base = device_friendly_name(dev);
    char value[64];
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t &ep = dev.endpoints[i];
        if (!endpoint_matches_filter(ep, endpoint_filter)) {
            continue;
        }
        if (attribute == "state" && ep.has_on_off) {
            return (publish_text(base + "/state", ep.on_off ? "ON" : "OFF", retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
        if (attribute == "temperature" && ep.has_temperature) {
            snprintf(value, sizeof(value), "%.2f", (double)ep.temperature_0_01_c / 100.0);
            return (publish_text(base + "/temperature", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
        if (attribute == "humidity" && ep.has_humidity) {
            snprintf(value, sizeof(value), "%.2f", (double)ep.humidity_0_01_pct / 100.0);
            return (publish_text(base + "/humidity", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
        if (attribute == "battery" && ep.has_power_battery_pct) {
            snprintf(value, sizeof(value), "%u", (unsigned)ep.battery_pct);
            return (publish_text(base + "/battery", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
        if (attribute == "occupancy" && ep.has_occupancy) {
            return (publish_text(base + "/occupancy", (ep.occupancy_bitmap & 0x01U) != 0U ? "true" : "false", retain, 1) >= 0)
                       ? ESP_OK
                       : ESP_FAIL;
        }
        if (attribute == "illuminance" && ep.has_illuminance) {
            snprintf(value, sizeof(value), "%u", (unsigned)ep.illuminance_measured_value);
            return (publish_text(base + "/illuminance", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
        if (attribute == "pressure" && ep.has_pressure) {
            snprintf(value, sizeof(value), "%.2f", (double)ep.pressure_0_1_kpa / 10.0);
            return (publish_text(base + "/pressure", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
        }
    }
    if (attribute == "linkquality") {
        snprintf(value, sizeof(value), "%u", (unsigned)dev.lqi);
        return (publish_text(base + "/linkquality", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
    }
    if (attribute == "last_seen_s") {
        snprintf(value, sizeof(value), "%.3f", dev.last_seen_s);
        return (publish_text(base + "/last_seen_s", value, retain, 1) >= 0) ? ESP_OK : ESP_FAIL;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t Bridge::publish_device_state(const device_record_t &dev, bool retain, int endpoint_filter)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria estado dispositivo");
    bool any_value = false;
    const std::string base = device_friendly_name(dev);
    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t &ep = dev.endpoints[i];
        if (!endpoint_matches_filter(ep, endpoint_filter)) {
            continue;
        }
        if (ep.has_on_off) {
            cJSON_AddStringToObject(root, "state", ep.on_off ? "ON" : "OFF");
            any_value = true;
        }
        if (ep.has_temperature) {
            cJSON_AddNumberToObject(root, "temperature", (double)ep.temperature_0_01_c / 100.0);
            any_value = true;
        }
        if (ep.has_humidity) {
            cJSON_AddNumberToObject(root, "humidity", (double)ep.humidity_0_01_pct / 100.0);
            any_value = true;
        }
        if (ep.has_power_battery_pct) {
            cJSON_AddNumberToObject(root, "battery", ep.battery_pct);
            any_value = true;
        }
        if (ep.has_occupancy) {
            cJSON_AddBoolToObject(root, "occupancy", (ep.occupancy_bitmap & 0x01U) != 0U);
            any_value = true;
        }
        if (ep.has_illuminance) {
            cJSON_AddNumberToObject(root, "illuminance", ep.illuminance_measured_value);
            any_value = true;
        }
        if (ep.has_pressure) {
            cJSON_AddNumberToObject(root, "pressure", (double)ep.pressure_0_1_kpa / 10.0);
            any_value = true;
        }
    }
    cJSON_AddNumberToObject(root, "linkquality", dev.lqi);
    cJSON_AddNumberToObject(root, "last_seen_s", dev.last_seen_s);
    if (kIncludeDeviceInformation) {
        cJSON *device = cJSON_CreateObject();
        ESP_RETURN_ON_FALSE(device != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria estado dispositivo.device");
        add_device_ref_json(device, &dev);
        cJSON_AddItemToObject(root, "device", device);
    }
    if (!any_value) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(publish_json(base, root, retain, 1), TAG, "No se pudo publicar estado");
    cJSON *availability = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(availability != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria availability");
    cJSON_AddStringToObject(availability, "state", (dev.in_network && !dev.silent) ? "online" : "offline");
    return publish_json(base + "/availability", availability, retain, 1);
}

esp_err_t Bridge::publish_selected_device_state(uint64_t ieee, uint16_t short_addr, bool retain)
{
    device_record_t dev = {};
    if ((ieee != 0U && load_device_by_ieee(ieee, &dev)) || (short_addr != 0U && load_device_by_short(short_addr, &dev))) {
        return publish_device_state(dev, retain);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t Bridge::publish_all_device_states(bool retain)
{
    device_record_t dev = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        const esp_err_t err = publish_device_state(dev, retain);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "publish_all_device_states(%d): No se pudo publicar device state", __LINE__);
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t Bridge::publish_bridge_devices(bool retain)
{
    cJSON *arr = cJSON_CreateArray();
    ESP_RETURN_ON_FALSE(arr != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/devices");
    device_record_t dev = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev)) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (item == nullptr) {
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }
        add_device_ref_json(item, &dev);
        cJSON_AddBoolToObject(item, "in_network", dev.in_network);
        cJSON_AddBoolToObject(item, "supported", device_definition_supported(dev));
        cJSON_AddItemToArray(arr, item);
    }
    return publish_json("bridge/devices", arr, retain, 1);
}

esp_err_t Bridge::publish_bridge_groups(bool retain)
{
    cJSON *arr = cJSON_CreateArray();
    ESP_RETURN_ON_FALSE(arr != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/groups");
    return publish_json("bridge/groups", arr, retain, 1);
}

esp_err_t Bridge::publish_bridge_definitions(bool retain)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/definitions");
    cJSON *actions = cJSON_CreateArray();
    cJSON *clusters = cJSON_CreateArray();
    cJSON *custom = cJSON_CreateArray();
    ESP_RETURN_ON_FALSE(actions != nullptr && clusters != nullptr && custom != nullptr,
                        ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/definitions arrays");
    cJSON_AddItemToArray(actions, cJSON_CreateString("state"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0000"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0001"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0006"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0402"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0405"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0406"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0400"));
    cJSON_AddItemToArray(clusters, cJSON_CreateString("0x0403"));
    cJSON_AddItemToArray(custom, cJSON_CreateString("0xFC57"));
    cJSON_AddItemToArray(custom, cJSON_CreateString("0xFC11"));
    cJSON_AddItemToObject(root, "actions", actions);
    cJSON_AddItemToObject(root, "clusters", clusters);
    cJSON_AddItemToObject(root, "custom_clusters", custom);
    return publish_json("bridge/definitions", root, retain, 1);
}

esp_err_t Bridge::publish_bridge_health(bool retain)
{
    device_table_network_summary_t summary = {};
    device_table_telemetry_t telemetry = {};
    device_table_get_network_summary(&summary);
    device_table_get_telemetry(&telemetry);

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/health");
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "nodes_total", summary.nodes_total);
    cJSON_AddNumberToObject(root, "nodes_silent_temp", summary.nodes_silent_temp);
    cJSON_AddNumberToObject(root, "nodes_absent_prolonged", summary.nodes_absent_prolonged);
    cJSON_AddNumberToObject(root, "silent_nodes", telemetry.silent_nodes);
    cJSON_AddNumberToObject(root, "report_attr_ok", telemetry.report_attr_ok);
    cJSON_AddNumberToObject(root, "report_attr_unchanged", telemetry.report_attr_unchanged);
    cJSON_AddNumberToObject(root, "zdo_latency_avg_ms", telemetry.zdo_latency_avg_ms);
    cJSON_AddNumberToObject(root, "zcl_latency_avg_ms", telemetry.zcl_latency_avg_ms);
    cJSON_AddNumberToObject(root, "mqtt_queue_depth", inbound_queue_ != nullptr ? uxQueueMessagesWaiting(inbound_queue_) : 0);
    return publish_json("bridge/health", root, retain, 1);
}

esp_err_t Bridge::publish_bridge_event(const zb_coord_event_info_t *event_info)
{
    const char *event_name = (event_info != nullptr && event_info->name[0] != '\0') ? event_info->name : "UNKNOWN";
    device_record_t event_dev = {};
    bool has_event_dev = false;
    if (event_info != nullptr && event_info->has_device) {
        if (event_info->ieee != 0U) {
            has_event_dev = load_device_by_ieee(event_info->ieee, &event_dev);
        }
        if (!has_event_dev && event_info->short_addr != 0U) {
            has_event_dev = load_device_by_short(event_info->short_addr, &event_dev);
        }
    }
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/event");
    const char *type = "device_announce";
    if (strcmp(event_name, "DEVICE_UPDATE") == 0) {
        type = "device_joined";
    } else if (strcmp(event_name, "DEVICE_LEAVE") == 0) {
        type = "device_leave";
    } else if (strcmp(event_name, "INTERVIEW_START") == 0 || strcmp(event_name, "INTERVIEW_SIMPLE_DESC") == 0 ||
               strcmp(event_name, "DEVICE_AUTH_FAILED") == 0) {
        type = "device_interview";
    }
    cJSON_AddStringToObject(root, "type", type);
    cJSON *data = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(data != nullptr, ESP_ERR_NO_MEM, TAG, "Sin memoria bridge/event.data");
    if (has_event_dev) {
        add_device_ref_json(data, &event_dev);
    } else {
        add_event_ref_json(data, event_info);
    }
    if (event_info != nullptr) {
        if (event_info->has_status) {
            cJSON_AddStringToObject(data, "status", event_info->status);
        }
    }
    if (strcmp(type, "device_interview") == 0 && has_event_dev) {
        cJSON_AddBoolToObject(data, "supported", device_definition_supported(event_dev));
        cJSON *definition = build_device_definition_json(event_dev);
        if (definition != nullptr) {
            cJSON_AddItemToObject(data, "definition", definition);
        }
    }
    cJSON_AddItemToObject(root, "data", data);
    return publish_json("bridge/event", root, false, 1);
}

esp_err_t Bridge::publish_response(const char *suffix, cJSON *json)
{
    return publish_json(std::string("bridge/response/") + suffix, json, false, 1);
}

esp_err_t Bridge::send_on_off_command(const device_record_t &dev, uint8_t endpoint, bool on)
{
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_zb_zcl_on_off_cmd_t cmd_req = {};
    cmd_req.zcl_basic_cmd.src_endpoint = kCoordinatorEndpoint;
    cmd_req.zcl_basic_cmd.dst_addr_u.addr_short = dev.short_addr;
    cmd_req.zcl_basic_cmd.dst_endpoint = endpoint;
    cmd_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd_req.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    const uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "[T+%07.3f] MQTT -> Zigbee on_off short=0x%04X ep=%u cmd=%s tsn=0x%02X",
             timebase_now_s(), dev.short_addr, endpoint, on ? "ON" : "OFF", tsn);
    return ESP_OK;
}

void Bridge::queue_full_publish()
{
    publish_info_pending_ = true;
    publish_devices_pending_ = true;
    publish_groups_pending_ = true;
    publish_definitions_pending_ = true;
    publish_states_pending_ = true;
}

void Bridge::handle_bridge_request(const std::string &request, const char *payload)
{
    if (request == "permit_join") {
        bool enable = false;
        int time_s = 180;
        cJSON *resp = cJSON_CreateObject();
        if (resp == nullptr) {
            return;
        }
        if (!parse_bool_token(trim_copy(payload).c_str(), &enable)) {
            cJSON *root = cJSON_ParseWithLength(payload, strlen(payload));
            if (root != nullptr) {
                const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
                if (cJSON_IsBool(value)) {
                    enable = cJSON_IsTrue(value);
                    const cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");
                    if (cJSON_IsNumber(time) && time->valuedouble > 0.0) {
                        time_s = (int)time->valuedouble;
                    }
                } else {
                    cJSON_Delete(root);
                    cJSON_AddBoolToObject(resp, "ok", false);
                    cJSON_AddStringToObject(resp, "error", "payload invalido");
                    (void)publish_response("permit_join", resp);
                    return;
                }
                cJSON_Delete(root);
            } else {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "payload invalido");
                (void)publish_response("permit_join", resp);
                return;
            }
        }
        const esp_err_t err = zb_coordinator_set_permit_join(enable);
        cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
        cJSON_AddBoolToObject(resp, "value", enable);
        cJSON_AddNumberToObject(resp, "time", enable ? time_s : 0);
        cJSON_AddStringToObject(resp, "status", (err == ESP_OK) ? "accepted" : esp_err_to_name(err));
        if (err == ESP_OK) {
            permit_join_open_ = enable;
            permit_join_deadline_ = enable ? (xTaskGetTickCount() + pdMS_TO_TICKS(time_s * 1000)) : 0;
            publish_info_pending_ = true;
        }
        (void)publish_response("permit_join", resp);
        return;
    }

    if (request == "restart") {
        cJSON *resp = cJSON_CreateObject();
        if (resp != nullptr) {
            cJSON_AddBoolToObject(resp, "ok", true);
            cJSON_AddStringToObject(resp, "status", "restarting");
            (void)publish_response("restart", resp);
        }
        restart_pending_ = true;
        restart_deadline_ = xTaskGetTickCount() + kRestartDelayTicks;
        return;
    }

    if (request == "health_check") {
        (void)publish_bridge_health(true);
        cJSON *resp = cJSON_CreateObject();
        if (resp != nullptr) {
            cJSON_AddBoolToObject(resp, "ok", true);
            cJSON_AddStringToObject(resp, "status", "published");
            (void)publish_response("health_check", resp);
        }
        return;
    }

    if (request == "options") {
        cJSON *resp = cJSON_CreateObject();
        if (resp == nullptr) {
            return;
        }
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "output", kOutputMode);
        cJSON_AddBoolToObject(resp, "availability", kAvailabilityEnabled);
        cJSON_AddBoolToObject(resp, "include_device_information", kIncludeDeviceInformation);
        cJSON_AddNumberToObject(resp, "health_interval_s", (double)kHealthPublishPeriodTicks / configTICK_RATE_HZ);
        (void)publish_response("options", resp);
        return;
    }

    if (request == "networkmap") {
        std::string map_type;
        if (!parse_networkmap_format(payload, &map_type)) {
            cJSON *resp = cJSON_CreateObject();
            if (resp != nullptr) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "payload invalido");
                (void)publish_response("networkmap", resp);
            }
            return;
        }
        cJSON *resp = cJSON_CreateObject();
        if (resp == nullptr) {
            return;
        }
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "type", map_type.c_str());
        if (map_type == "raw") {
            cJSON *map = build_networkmap_array();
            if (map == nullptr) {
                cJSON_Delete(resp);
                return;
            }
            cJSON_AddItemToObject(resp, "nodes", map);
        } else {
            const std::string rendered = (map_type == "graphviz") ? build_networkmap_graphviz() : build_networkmap_plantuml();
            cJSON_AddStringToObject(resp, "data", rendered.c_str());
        }
        (void)publish_response("networkmap", resp);
        return;
    }

    if (request == "device/configure") {
        cJSON *root = cJSON_ParseWithLength(payload, strlen(payload));
        cJSON *resp = cJSON_CreateObject();
        if (resp == nullptr) {
            cJSON_Delete(root);
            return;
        }
        if (root == nullptr) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "payload invalido");
            (void)publish_response("device/configure", resp);
            return;
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(name) || name->valuestring == nullptr) {
            name = cJSON_GetObjectItemCaseSensitive(root, "friendly_name");
        }
        device_record_t dev = {};
        const bool found = cJSON_IsString(name) && name->valuestring != nullptr && load_device_by_name(name->valuestring, &dev);
        if (!found) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "dispositivo no encontrado");
            (void)publish_response("device/configure", resp);
            cJSON_Delete(root);
            return;
        }
        const esp_err_t err = zb_coordinator_request_interview(dev.short_addr);
        cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
        cJSON_AddStringToObject(resp, "status", (err == ESP_OK) ? "accepted" : esp_err_to_name(err));
        cJSON_AddStringToObject(resp, "friendly_name", device_friendly_name(dev).c_str());
        (void)publish_response("device/configure", resp);
        cJSON_Delete(root);
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp != nullptr) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "request no soportada por este firmware");
        (void)publish_response(request.c_str(), resp);
    }
}

void Bridge::handle_entity_request(const entity_request_t &req, const char *payload)
{
    device_record_t dev = {};
    if (!load_device_by_name(req.name, &dev)) {
        (void)publish_bridge_log("warn", TAG, "MQTT: entidad no encontrada para " + req.name);
        return;
    }
    if (req.action == "get") {
        (void)publish_device_state(dev, true, req.has_endpoint ? req.endpoint : -1);
        return;
    }
    const std::string attribute = req.attribute.empty() ? "state" : req.attribute;
    if (attribute != "state") {
        (void)publish_bridge_log("warn", TAG, "MQTT: atributo set no soportado para " + req.name + ": " + attribute);
        return;
    }
    bool on = false;
    if (!parse_state_payload(payload, &on)) {
        (void)publish_bridge_log("warn", TAG, "MQTT: payload invalido para set/state en " + req.name);
        return;
    }
    uint8_t endpoint = 0;
    if (req.has_endpoint) {
        endpoint = req.endpoint;
        bool found = false;
        for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
            const device_endpoint_record_t &ep = dev.endpoints[i];
            if (ep.used && ep.endpoint_id == endpoint && endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)) {
                found = true;
                break;
            }
        }
        if (!found) {
            (void)publish_bridge_log("warn", TAG, "MQTT: endpoint sin cluster OnOff para " + req.name);
            return;
        }
    } else if (!find_on_off_endpoint(dev, &endpoint)) {
        (void)publish_bridge_log("warn", TAG, "MQTT: entidad sin cluster OnOff para " + req.name);
        return;
    }
    const esp_err_t err = send_on_off_command(dev, endpoint, on);
    if (err != ESP_OK) {
        (void)publish_bridge_log("error", TAG, "MQTT: fallo enviando set/state a " + req.name + " -> " + esp_err_to_name(err));
    }
}

void Bridge::handle_message(const mqtt_inbound_msg_t &msg)
{
    const std::string topic(msg.topic);
    const std::string base(CONFIG_MQTT_BRIDGE_BASE_TOPIC);
    if (topic.size() <= base.size() || topic.compare(0, base.size(), base) != 0 || topic[base.size()] != '/') {
        return;
    }
    const std::string rel = topic.substr(base.size() + 1U);
    if (rel.rfind("bridge/request/", 0) == 0) {
        handle_bridge_request(rel.substr(strlen("bridge/request/")), msg.payload);
        return;
    }
    entity_request_t req = {};
    if (!parse_entity_request(rel, &req)) {
        return;
    }
    handle_entity_request(req, msg.payload);
}

void Bridge::process_inbound()
{
    if (inbound_queue_ == nullptr) {
        return;
    }
    mqtt_inbound_msg_t msg = {};
    while (xQueueReceive(inbound_queue_, &msg, 0) == pdTRUE) {
        handle_message(msg);
    }
}

void Bridge::process_events()
{
    if (zigbee_event_queue_ == nullptr) {
        return;
    }
    zb_coord_event_info_t evt_buf[kPendingEventMax] = {};
    size_t evt_count = 0;
    while (evt_count < kPendingEventMax && xQueueReceive(zigbee_event_queue_, &evt_buf[evt_count], 0) == pdTRUE) {
        evt_count++;
    }
    if (!mqtt_connected_) {
        return;
    }
    for (size_t i = 0; i < evt_count; ++i) {
        const zb_coord_event_info_t *evt = &evt_buf[i];
        const char *evt_name = evt->name;
        if (event_should_publish_bridge_event(evt_name)) {
            (void)publish_bridge_event(evt);
        }
        if (strcmp(evt_name, "PERMIT_JOIN_OPEN") == 0) {
            permit_join_open_ = true;
            permit_join_deadline_ = xTaskGetTickCount() + kDefaultPermitJoinTicks;
            publish_info_pending_ = true;
        } else if (strcmp(evt_name, "PERMIT_JOIN_CLOSED") == 0) {
            permit_join_open_ = false;
            permit_join_deadline_ = 0;
            publish_info_pending_ = true;
        } else if (event_should_refresh_device_state(evt_name) && evt->has_device) {
            const esp_err_t err = publish_selected_device_state(evt->ieee, evt->short_addr, true);
            if (strcmp(evt_name, "DEVICE_REPORT") == 0) {
                const double age_ms = (timebase_now_s() - evt->ts_s) * 1000.0;
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "[T+%07.3f] MQTT state refresh OK short=0x%04X ieee=0x%016llX age=%.1f ms", timebase_now_s(),
                             evt->short_addr, (unsigned long long)evt->ieee, age_ms);
                } else {
                    ESP_LOGW(TAG, "[T+%07.3f] MQTT state refresh FAIL short=0x%04X ieee=0x%016llX age=%.1f ms err=%s", timebase_now_s(),
                             evt->short_addr, (unsigned long long)evt->ieee, age_ms, esp_err_to_name(err));
                }
            }
            if (strcmp(evt_name, "DEVICE_ANNOUNCE") == 0 || strcmp(evt_name, "DEVICE_UPDATE") == 0 ||
                strcmp(evt_name, "DEVICE_LEAVE") == 0 || strcmp(evt_name, "INTERVIEW_SIMPLE_DESC") == 0) {
                publish_devices_pending_ = true;
            }
        } else if (strstr(evt_name, "NETWORK_") == evt_name || strcmp(evt_name, "STACK_STARTED") == 0) {
            queue_full_publish();
        }
    }
}

esp_err_t Bridge::init()
{
    if (initialized_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ensure_network_stack(), TAG, "No se pudo preparar esp_netif/esp_event");
    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "No se pudieron registrar eventos");
    ESP_RETURN_ON_ERROR(create_inbound_queue(), TAG, "No se pudo crear cola MQTT");
    ESP_RETURN_ON_ERROR(create_event_queue(), TAG, "No se pudo crear cola eventos Zigbee");
    ESP_RETURN_ON_ERROR(init_ethernet(), TAG, "No se pudo inicializar ENC28J60");
    ESP_RETURN_ON_ERROR(attach_netif(), TAG, "No se pudo adjuntar netif Ethernet");
    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle_), TAG, "No se pudo arrancar esp_eth");
    initialized_ = true;
    ESP_RETURN_ON_ERROR(create_worker_task(), TAG, "No se pudo arrancar tarea mqtt_bridge");
    ESP_LOGI(TAG, "[T+%07.3f] Bridge listo: Ethernet SPI ENC28J60 arrancado, esperando DHCP", timebase_now_s());
    return ESP_OK;
}

void Bridge::poll()
{
    process_inbound();
    process_events();
    const TickType_t now = xTaskGetTickCount();
    if (permit_join_open_ && permit_join_deadline_ != 0 && (int32_t)(now - permit_join_deadline_) >= 0) {
        if (zb_coordinator_set_permit_join(false) == ESP_OK) {
            permit_join_open_ = false;
            permit_join_deadline_ = 0;
            publish_info_pending_ = true;
        }
    }
    if (!mqtt_connected_) {
        next_health_tick_ = 0;
        return;
    }
    if (publish_online_pending_) {
        (void)publish_bridge_state("online");
        publish_online_pending_ = false;
    }
    if (publish_info_pending_) {
        (void)publish_bridge_info(true);
        publish_info_pending_ = false;
    }
    if (publish_devices_pending_) {
        (void)publish_bridge_devices(true);
        publish_devices_pending_ = false;
    }
    if (publish_groups_pending_) {
        (void)publish_bridge_groups(true);
        publish_groups_pending_ = false;
    }
    if (publish_definitions_pending_) {
        (void)publish_bridge_definitions(true);
        publish_definitions_pending_ = false;
    }
    if (publish_states_pending_) {
        (void)publish_all_device_states(true);
        publish_states_pending_ = false;
    }
    if (next_health_tick_ == 0 || (int32_t)(now - next_health_tick_) >= 0) {
        (void)publish_bridge_health(true);
        next_health_tick_ = now + kHealthPublishPeriodTicks;
    }
    if (restart_pending_ && restart_deadline_ != 0 && (int32_t)(now - restart_deadline_) >= 0) {
        esp_restart();
    }
}

esp_err_t Bridge::notify_zigbee_event(const char *event_name)
{
    if (event_name == nullptr || zigbee_event_queue_ == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    zb_coord_event_info_t info = {};
    if (zb_coordinator_get_last_event_info(&info) != ESP_OK || strcmp(info.name, event_name) != 0) {
        snprintf(info.name, sizeof(info.name), "%s", event_name);
        info.ts_s = timebase_now_s();
    }
    if (xQueueSend(zigbee_event_queue_, &info, 0) != pdTRUE) {
        zb_coord_event_info_t dropped = {};
        (void)xQueueReceive(zigbee_event_queue_, &dropped, 0);
        if (xQueueSend(zigbee_event_queue_, &info, 0) != pdTRUE) {
            ESP_LOGW(TAG, "[T+%07.3f] Cola eventos Zigbee->MQTT saturada, evento descartado: %s", timebase_now_s(), event_name);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "[T+%07.3f] Cola eventos Zigbee->MQTT saturada, descartado evento antiguo para insertar %s", timebase_now_s(), event_name);
    }
    wake_worker();
    return ESP_OK;
}

void Bridge::bridge_task_entry(void *arg)
{
    Bridge *self = static_cast<Bridge *>(arg);
    while (self != nullptr) {
        self->poll();
        (void)ulTaskNotifyTake(pdTRUE, kBridgePollPeriodTicks);
    }
    vTaskDelete(nullptr);
}

void Bridge::on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    Bridge *self = static_cast<Bridge *>(arg);
    if (self == nullptr || event_data == nullptr) {
        return;
    }
    uint8_t mac_addr[ETH_ADDR_LEN] = {};
    esp_eth_handle_t eth_handle = *static_cast<esp_eth_handle_t *>(event_data);
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
            ESP_LOGI(TAG, "[T+%07.3f] Ethernet link UP, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     timebase_now_s(), mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        self->mqtt_connected_ = false;
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet link DOWN", timebase_now_s());
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[T+%07.3f] Ethernet START", timebase_now_s());
        break;
    case ETHERNET_EVENT_STOP:
        self->mqtt_connected_ = false;
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet STOP", timebase_now_s());
        break;
    default:
        break;
    }
}

void Bridge::on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    Bridge *self = static_cast<Bridge *>(arg);
    if (self == nullptr || event_data == nullptr) {
        return;
    }
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        if (event->esp_netif != self->netif_) {
            return;
        }
        ESP_LOGI(TAG, "[T+%07.3f] DHCP OK - IP " IPSTR " MASK " IPSTR " GW " IPSTR,
                 timebase_now_s(), IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
        if (self->start_mdns() == ESP_OK) {
            (void)self->start_mqtt();
        }
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        self->mqtt_connected_ = false;
        ESP_LOGW(TAG, "[T+%07.3f] Ethernet ha perdido la IP DHCP", timebase_now_s());
    }
}

void Bridge::on_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    Bridge *self = static_cast<Bridge *>(arg);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (self == nullptr || event == nullptr) {
        return;
    }
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        self->mqtt_connected_ = true;
        self->publish_online_pending_ = true;
        self->queue_full_publish();
        const std::string sub = full_topic("#");
        const int msg_id = esp_mqtt_client_subscribe(self->mqtt_client_, sub.c_str(), 1);
        ESP_LOGI(TAG, "[T+%07.3f] MQTT conectado y suscrito a %s (msg_id=%d)", timebase_now_s(), sub.c_str(), msg_id);
        self->wake_worker();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        self->mqtt_connected_ = false;
        ESP_LOGW(TAG, "[T+%07.3f] MQTT desconectado", timebase_now_s());
        self->wake_worker();
        break;
    case MQTT_EVENT_DATA:
        if (self->inbound_queue_ != nullptr) {
            mqtt_inbound_msg_t msg = {};
            const int topic_len = (event->topic_len < (int)sizeof(msg.topic) - 1) ? event->topic_len : (int)sizeof(msg.topic) - 1;
            const int data_len = (event->data_len < (int)sizeof(msg.payload) - 1) ? event->data_len : (int)sizeof(msg.payload) - 1;
            memcpy(msg.topic, event->topic, topic_len);
            msg.topic[topic_len] = '\0';
            memcpy(msg.payload, event->data, data_len);
            msg.payload[data_len] = '\0';
            (void)xQueueSend(self->inbound_queue_, &msg, 0);
            self->wake_worker();
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "[T+%07.3f] MQTT error", timebase_now_s());
        break;
    default:
        break;
    }
}

} // namespace mqtt_bridge

extern "C" esp_err_t mqtt_bridge_init(void)
{
    return mqtt_bridge::s_bridge.init();
}

extern "C" void mqtt_bridge_poll(void)
{
    /* El bridge se atiende desde su propia tarea; esta API queda como compatibilidad vacia. */
}

extern "C" esp_err_t mqtt_bridge_notify_zigbee_event(const char *event_name)
{
    return mqtt_bridge::s_bridge.notify_zigbee_event(event_name);
}
