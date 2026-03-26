#include "matter_bridge.h"

#include <inttypes.h>
#include <string.h>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_bridge.h>
#include <esp_zigbee_core.h>
#include <platform/CHIPDeviceLayer.h>

#include "bridge_core.h"
#include "device_table.h"
#include "timebase.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

static const char *TAG = "matter_bridge";
static constexpr uint8_t kCoordinatorEndpoint = 1;
static constexpr TickType_t kSyncPeriodTicks = pdMS_TO_TICKS(250);

using namespace chip::app::Clusters;
using namespace esp_matter;

typedef struct {
    bool occupied;
    uint64_t ieee;
    uint16_t matter_endpoint_id;
    uint16_t zigbee_short_addr;
    uint8_t zigbee_endpoint_id;
    uint8_t reachable;
    uint32_t expose_mask;
    uint32_t matter_device_type_id;
    esp_matter_bridge::device_t *device;
} matter_bridge_slot_t;

static node_t *s_node = nullptr;
static endpoint_t *s_aggregator = nullptr;
static uint16_t s_aggregator_endpoint_id = chip::kInvalidEndpointId;
static matter_bridge_slot_t s_slots[DEVICE_TABLE_MAX_DEVICES];
static bool s_started = false;
static bool s_sync_requested = true;
static bool s_sync_scheduled = false;
static TickType_t s_last_sync_tick = 0;

static bool endpoint_has_cluster(const device_endpoint_record_t *ep, uint16_t cluster_id)
{
    if (ep == nullptr) {
        return false;
    }
    for (size_t i = 0; i < ep->input_clusters_len; ++i) {
        if (ep->input_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static const device_endpoint_record_t *find_endpoint_for_mask(const device_record_t *dev, uint32_t mask)
{
    if (dev == nullptr) {
        return nullptr;
    }

    for (size_t i = 0; i < DEVICE_TABLE_MAX_ENDPOINTS; ++i) {
        const device_endpoint_record_t *ep = &dev->endpoints[i];
        if (!ep->used) {
            continue;
        }
        if ((mask & BRIDGE_EXPOSE_ON_OFF) != 0U &&
            (ep->has_on_off || endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF))) {
            return ep;
        }
        if ((mask & BRIDGE_EXPOSE_TEMPERATURE) != 0U &&
            (ep->has_temperature || endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT))) {
            return ep;
        }
        if ((mask & BRIDGE_EXPOSE_HUMIDITY) != 0U &&
            (ep->has_humidity || endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT))) {
            return ep;
        }
        if ((mask & BRIDGE_EXPOSE_OCCUPANCY) != 0U &&
            (ep->has_occupancy || endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING))) {
            return ep;
        }
        if ((mask & BRIDGE_EXPOSE_ILLUMINANCE) != 0U &&
            (ep->has_illuminance || endpoint_has_cluster(ep, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT))) {
            return ep;
        }
    }
    return nullptr;
}

static bool load_device_by_ieee(uint64_t ieee, device_record_t *out)
{
    if (out == nullptr) {
        return false;
    }
    device_record_t dev = {};
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!device_table_copy_device_at(i, &dev) || !dev.occupied) {
            continue;
        }
        if (dev.ieee == ieee) {
            *out = dev;
            return true;
        }
    }
    return false;
}

static uint32_t primary_device_type_from_mask(uint32_t expose_mask)
{
    if ((expose_mask & BRIDGE_EXPOSE_ON_OFF) != 0U) {
        return endpoint::on_off_light::get_device_type_id();
    }
    if ((expose_mask & BRIDGE_EXPOSE_OCCUPANCY) != 0U) {
        return endpoint::occupancy_sensor::get_device_type_id();
    }
    if ((expose_mask & BRIDGE_EXPOSE_TEMPERATURE) != 0U) {
        return endpoint::temperature_sensor::get_device_type_id();
    }
    if ((expose_mask & BRIDGE_EXPOSE_HUMIDITY) != 0U) {
        return endpoint::humidity_sensor::get_device_type_id();
    }
    if ((expose_mask & BRIDGE_EXPOSE_ILLUMINANCE) != 0U) {
        return endpoint::light_sensor::get_device_type_id();
    }
    return 0U;
}

static matter_bridge_slot_t *find_slot_by_ieee(uint64_t ieee)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_slots[i].occupied && s_slots[i].ieee == ieee) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

static matter_bridge_slot_t *find_slot_by_endpoint(uint16_t matter_endpoint_id)
{
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_slots[i].occupied && s_slots[i].matter_endpoint_id == matter_endpoint_id) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

static matter_bridge_slot_t *alloc_slot(uint64_t ieee)
{
    matter_bridge_slot_t *slot = find_slot_by_ieee(ieee);
    if (slot != nullptr) {
        return slot;
    }
    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (!s_slots[i].occupied) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].occupied = true;
            s_slots[i].ieee = ieee;
            return &s_slots[i];
        }
    }
    return nullptr;
}

static void remove_slot(matter_bridge_slot_t *slot)
{
    if (slot == nullptr) {
        return;
    }
    if (slot->device != nullptr) {
        ESP_LOGW(TAG, "[T+%07.3f] Matter remove ieee=0x%016" PRIX64 " endpoint=%u",
                 timebase_now_s(), slot->ieee, (unsigned)slot->matter_endpoint_id);
        (void)esp_matter_bridge::remove_device(slot->device);
    }
    memset(slot, 0, sizeof(*slot));
}

static esp_err_t report_string_attr(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, const char *value)
{
    char buffer[BRIDGE_FRIENDLY_NAME_MAX];
    const char *src = (value != nullptr) ? value : "";
    const size_t len = strnlen(src, sizeof(buffer) - 1U);
    memcpy(buffer, src, len);
    buffer[len] = '\0';
    esp_matter_attr_val_t attr = esp_matter_char_str(buffer, (uint16_t)len);
    return attribute::report(endpoint_id, cluster_id, attribute_id, &attr);
}

static esp_err_t report_bool_attr(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, bool value)
{
    esp_matter_attr_val_t attr = esp_matter_bool(value);
    return attribute::report(endpoint_id, cluster_id, attribute_id, &attr);
}

static esp_err_t report_i16_attr(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, int16_t value)
{
    esp_matter_attr_val_t attr = esp_matter_int16(value);
    return attribute::report(endpoint_id, cluster_id, attribute_id, &attr);
}

static esp_err_t report_u16_attr(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, uint16_t value)
{
    esp_matter_attr_val_t attr = esp_matter_uint16(value);
    return attribute::report(endpoint_id, cluster_id, attribute_id, &attr);
}

static esp_err_t report_u8_attr(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, uint8_t value)
{
    esp_matter_attr_val_t attr = esp_matter_bitmap8(value);
    return attribute::report(endpoint_id, cluster_id, attribute_id, &attr);
}

static void open_commissioning_window_if_necessary()
{
    VerifyOrReturn(chip::Server::GetInstance().GetFabricTable().FabricCount() == 0);

    chip::CommissioningWindowManager &commission_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    VerifyOrReturn(!commission_mgr.IsCommissioningWindowOpen());

    CHIP_ERROR err = commission_mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                                                 chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "No se pudo abrir ventana de commissioning: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "[T+%07.3f] Matter commissioning completo", timebase_now_s());
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "[T+%07.3f] Matter commissioning session iniciada", timebase_now_s());
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "[T+%07.3f] Matter commissioning window abierta", timebase_now_s());
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "[T+%07.3f] Matter commissioning window cerrada", timebase_now_s());
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGW(TAG, "[T+%07.3f] Matter fabric eliminada", timebase_now_s());
        open_commissioning_window_if_necessary();
        break;
    default:
        break;
    }
}

static esp_err_t send_on_off_command(const matter_bridge_slot_t *slot, bool on)
{
    if (slot == nullptr || slot->zigbee_short_addr == 0U || slot->zigbee_endpoint_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_zb_zcl_on_off_cmd_t cmd_req = {};
    cmd_req.zcl_basic_cmd.src_endpoint = kCoordinatorEndpoint;
    cmd_req.zcl_basic_cmd.dst_addr_u.addr_short = slot->zigbee_short_addr;
    cmd_req.zcl_basic_cmd.dst_endpoint = slot->zigbee_endpoint_id;
    cmd_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd_req.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    const uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "[T+%07.3f] Matter -> Zigbee on_off short=0x%04X ep=%u cmd=%s tsn=0x%02X",
             timebase_now_s(), slot->zigbee_short_addr, slot->zigbee_endpoint_id, on ? "ON" : "OFF", tsn);
    return ESP_OK;
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    (void)priv_data;
    if (type != attribute::PRE_UPDATE || cluster_id != OnOff::Id ||
        attribute_id != OnOff::Attributes::OnOff::Id || val == nullptr) {
        return ESP_OK;
    }
    matter_bridge_slot_t *slot = find_slot_by_endpoint(endpoint_id);
    if (slot == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    return send_on_off_command(slot, val->val.b);
}

static esp_err_t identify_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                             uint8_t effect_variant, void *priv_data)
{
    (void)type;
    (void)endpoint_id;
    (void)effect_id;
    (void)effect_variant;
    (void)priv_data;
    return ESP_OK;
}

static esp_err_t create_device_type(endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    matter_bridge_slot_t *slot = static_cast<matter_bridge_slot_t *>(priv_data);
    if (slot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (device_type_id == endpoint::on_off_light::get_device_type_id()) {
        endpoint::on_off_light::config_t config;
        err = endpoint::on_off_light::add(ep, &config);
    } else if (device_type_id == endpoint::occupancy_sensor::get_device_type_id()) {
        endpoint::occupancy_sensor::config_t config;
        config.occupancy_sensing.occupancy_sensor_type =
            chip::to_underlying(OccupancySensing::OccupancySensorTypeEnum::kPir);
        config.occupancy_sensing.occupancy_sensor_type_bitmap =
            chip::to_underlying(OccupancySensing::OccupancySensorTypeBitmap::kPir);
        err = endpoint::occupancy_sensor::add(ep, &config);
    } else if (device_type_id == endpoint::temperature_sensor::get_device_type_id()) {
        endpoint::temperature_sensor::config_t config;
        err = endpoint::temperature_sensor::add(ep, &config);
    } else if (device_type_id == endpoint::humidity_sensor::get_device_type_id()) {
        endpoint::humidity_sensor::config_t config;
        err = endpoint::humidity_sensor::add(ep, &config);
    } else if (device_type_id == endpoint::light_sensor::get_device_type_id()) {
        endpoint::light_sensor::config_t config;
        err = endpoint::light_sensor::add(ep, &config);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "No se pudo anadir device type Matter");

    if ((slot->expose_mask & BRIDGE_EXPOSE_TEMPERATURE) != 0U &&
        device_type_id != endpoint::temperature_sensor::get_device_type_id()) {
        cluster::temperature_measurement::config_t config;
        ESP_RETURN_ON_FALSE(cluster::temperature_measurement::create(ep, &config, CLUSTER_FLAG_SERVER) != nullptr,
                            ESP_FAIL, TAG, "No se pudo anadir temperatura");
    }
    if ((slot->expose_mask & BRIDGE_EXPOSE_HUMIDITY) != 0U &&
        device_type_id != endpoint::humidity_sensor::get_device_type_id()) {
        cluster::relative_humidity_measurement::config_t config;
        ESP_RETURN_ON_FALSE(cluster::relative_humidity_measurement::create(ep, &config, CLUSTER_FLAG_SERVER) != nullptr,
                            ESP_FAIL, TAG, "No se pudo anadir humedad");
    }
    if ((slot->expose_mask & BRIDGE_EXPOSE_OCCUPANCY) != 0U &&
        device_type_id != endpoint::occupancy_sensor::get_device_type_id()) {
        cluster::occupancy_sensing::config_t config;
        config.occupancy_sensor_type = chip::to_underlying(OccupancySensing::OccupancySensorTypeEnum::kPir);
        config.occupancy_sensor_type_bitmap = chip::to_underlying(OccupancySensing::OccupancySensorTypeBitmap::kPir);
        ESP_RETURN_ON_FALSE(cluster::occupancy_sensing::create(ep, &config, CLUSTER_FLAG_SERVER) != nullptr,
                            ESP_FAIL, TAG, "No se pudo anadir ocupacion");
    }
    if ((slot->expose_mask & BRIDGE_EXPOSE_ILLUMINANCE) != 0U &&
        device_type_id != endpoint::light_sensor::get_device_type_id()) {
        cluster::illuminance_measurement::config_t config;
        ESP_RETURN_ON_FALSE(cluster::illuminance_measurement::create(ep, &config, CLUSTER_FLAG_SERVER) != nullptr,
                            ESP_FAIL, TAG, "No se pudo anadir iluminancia");
    }
    return ESP_OK;
}

static void prune_orphaned_bridge_entries()
{
    uint16_t endpoint_ids[MAX_BRIDGED_DEVICE_COUNT];
    if (esp_matter_bridge::get_bridged_endpoint_ids(endpoint_ids) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < MAX_BRIDGED_DEVICE_COUNT; ++i) {
        const uint16_t endpoint_id = endpoint_ids[i];
        if (endpoint_id == chip::kInvalidEndpointId) {
            continue;
        }

        bool found = false;
        for (size_t slot = 0; slot < DEVICE_TABLE_MAX_DEVICES; ++slot) {
            bridge_device_binding_t binding = {};
            if (!bridge_core_copy_binding_at(slot, &binding, nullptr)) {
                continue;
            }
            if (binding.matter_endpoint_id == endpoint_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "[T+%07.3f] Matter purge endpoint huerfano=%u", timebase_now_s(), (unsigned)endpoint_id);
            (void)esp_matter_bridge::erase_bridged_device_info(endpoint_id);
        }
    }
}

static void sync_slot_state(matter_bridge_slot_t *slot, const bridge_device_binding_t *binding,
                            const bridge_device_state_t *state, const device_record_t *dev)
{
    if (slot == nullptr || binding == nullptr || state == nullptr || slot->device == nullptr) {
        return;
    }

    const uint16_t endpoint_id = slot->matter_endpoint_id;
    (void)report_string_attr(endpoint_id, BridgedDeviceBasicInformation::Id,
                             BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, binding->friendly_name);
    (void)report_string_attr(endpoint_id, BridgedDeviceBasicInformation::Id,
                             BridgedDeviceBasicInformation::Attributes::UniqueID::Id, binding->matter_unique_id);
    (void)report_bool_attr(endpoint_id, BridgedDeviceBasicInformation::Id,
                           BridgedDeviceBasicInformation::Attributes::Reachable::Id, state->reachable != 0U);

    if (dev == nullptr) {
        return;
    }

    const device_endpoint_record_t *on_off_ep = find_endpoint_for_mask(dev, BRIDGE_EXPOSE_ON_OFF);
    if ((binding->expose_mask & BRIDGE_EXPOSE_ON_OFF) != 0U && on_off_ep != nullptr) {
        (void)report_bool_attr(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, on_off_ep->on_off);
    }

    const device_endpoint_record_t *temp_ep = find_endpoint_for_mask(dev, BRIDGE_EXPOSE_TEMPERATURE);
    if ((binding->expose_mask & BRIDGE_EXPOSE_TEMPERATURE) != 0U && temp_ep != nullptr && temp_ep->has_temperature) {
        (void)report_i16_attr(endpoint_id, TemperatureMeasurement::Id,
                              TemperatureMeasurement::Attributes::MeasuredValue::Id, temp_ep->temperature_0_01_c);
    }

    const device_endpoint_record_t *hum_ep = find_endpoint_for_mask(dev, BRIDGE_EXPOSE_HUMIDITY);
    if ((binding->expose_mask & BRIDGE_EXPOSE_HUMIDITY) != 0U && hum_ep != nullptr && hum_ep->has_humidity) {
        (void)report_u16_attr(endpoint_id, RelativeHumidityMeasurement::Id,
                              RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, hum_ep->humidity_0_01_pct);
    }

    const device_endpoint_record_t *occ_ep = find_endpoint_for_mask(dev, BRIDGE_EXPOSE_OCCUPANCY);
    if ((binding->expose_mask & BRIDGE_EXPOSE_OCCUPANCY) != 0U && occ_ep != nullptr && occ_ep->has_occupancy) {
        (void)report_u8_attr(endpoint_id, OccupancySensing::Id,
                             OccupancySensing::Attributes::Occupancy::Id, occ_ep->occupancy_bitmap);
    }

    const device_endpoint_record_t *lux_ep = find_endpoint_for_mask(dev, BRIDGE_EXPOSE_ILLUMINANCE);
    if ((binding->expose_mask & BRIDGE_EXPOSE_ILLUMINANCE) != 0U && lux_ep != nullptr && lux_ep->has_illuminance) {
        (void)report_u16_attr(endpoint_id, IlluminanceMeasurement::Id,
                              IlluminanceMeasurement::Attributes::MeasuredValue::Id, lux_ep->illuminance_measured_value);
    }
}

static void sync_from_bridge_core()
{
    bool seen[DEVICE_TABLE_MAX_DEVICES] = {};

    for (size_t slot_index = 0; slot_index < DEVICE_TABLE_MAX_DEVICES; ++slot_index) {
        bridge_device_binding_t binding = {};
        bridge_device_state_t state = {};
        if (!bridge_core_copy_binding_at(slot_index, &binding, &state) || binding.used == 0U) {
            continue;
        }
        if (binding.hidden != 0U || state.supported == 0U) {
            continue;
        }

        matter_bridge_slot_t *slot = alloc_slot(binding.zigbee_ieee);
        if (slot == nullptr) {
            ESP_LOGE(TAG, "[T+%07.3f] Sin slot Matter libre para ieee=0x%016" PRIX64, timebase_now_s(), binding.zigbee_ieee);
            continue;
        }

        device_record_t dev = {};
        const bool has_dev = load_device_by_ieee(binding.zigbee_ieee, &dev);
        const device_endpoint_record_t *primary_ep = has_dev ? find_endpoint_for_mask(&dev, binding.expose_mask) : nullptr;
        const uint32_t matter_device_type_id = primary_device_type_from_mask(binding.expose_mask);
        if (matter_device_type_id == 0U || primary_ep == nullptr) {
            continue;
        }

        slot->expose_mask = binding.expose_mask;
        slot->matter_device_type_id = matter_device_type_id;
        slot->zigbee_short_addr = state.zigbee_short_addr;
        slot->zigbee_endpoint_id = primary_ep->endpoint_id;
        slot->reachable = state.reachable;

        if (slot->device == nullptr) {
            esp_matter_bridge::device_t *device = nullptr;
            if (binding.matter_endpoint_id != 0U) {
                device = esp_matter_bridge::resume_device(s_node, binding.matter_endpoint_id, slot);
            }
            if (device == nullptr) {
                if (binding.matter_endpoint_id != 0U) {
                    (void)esp_matter_bridge::erase_bridged_device_info(binding.matter_endpoint_id);
                }
                device = esp_matter_bridge::create_device(s_node, s_aggregator_endpoint_id, matter_device_type_id, slot);
            }
            if (device == nullptr) {
                ESP_LOGE(TAG, "[T+%07.3f] No se pudo crear/resumir Matter para ieee=0x%016" PRIX64,
                         timebase_now_s(), binding.zigbee_ieee);
                continue;
            }
            slot->device = device;
            slot->matter_endpoint_id = endpoint::get_id(device->endpoint);
            (void)endpoint::enable(device->endpoint);
            (void)bridge_core_set_matter_endpoint_id(binding.zigbee_ieee, slot->matter_endpoint_id);
            ESP_LOGI(TAG, "[T+%07.3f] Matter bridge activo ieee=0x%016" PRIX64 " endpoint=%u type=0x%08" PRIX32,
                     timebase_now_s(), binding.zigbee_ieee, (unsigned)slot->matter_endpoint_id, matter_device_type_id);
        }

        seen[slot - s_slots] = true;
        sync_slot_state(slot, &binding, &state, has_dev ? &dev : nullptr);
    }

    for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
        if (s_slots[i].occupied && !seen[i]) {
            remove_slot(&s_slots[i]);
        }
    }
}

esp_err_t matter_bridge_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    node::config_t node_config;
    s_node = node::create(&node_config, attribute_update_cb, identify_cb);
    ESP_RETURN_ON_FALSE(s_node != nullptr, ESP_FAIL, TAG, "No se pudo crear nodo Matter");

    endpoint::aggregator::config_t aggregator_config;
    s_aggregator = endpoint::aggregator::create(s_node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    ESP_RETURN_ON_FALSE(s_aggregator != nullptr, ESP_FAIL, TAG, "No se pudo crear endpoint aggregator");
    s_aggregator_endpoint_id = endpoint::get_id(s_aggregator);

    ESP_RETURN_ON_ERROR(esp_matter::start(app_event_cb), TAG, "No se pudo arrancar Matter");
    ESP_RETURN_ON_ERROR(esp_matter_bridge::initialize(s_node, create_device_type), TAG,
                        "No se pudo inicializar esp_matter_bridge");

    prune_orphaned_bridge_entries();
    memset(s_slots, 0, sizeof(s_slots));
    s_started = true;
    s_sync_requested = true;
    s_last_sync_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "[T+%07.3f] Matter bridge inicializado aggregator=%u", timebase_now_s(),
             (unsigned)s_aggregator_endpoint_id);
    return ESP_OK;
}

void matter_bridge_poll(void)
{
    if (!s_started) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (!s_sync_requested && (now - s_last_sync_tick) < kSyncPeriodTicks) {
        return;
    }
    if (s_sync_scheduled) {
        return;
    }

    s_sync_requested = false;
    s_sync_scheduled = true;
    s_last_sync_tick = now;
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        sync_from_bridge_core();
        s_sync_scheduled = false;
    });
}

void matter_bridge_factory_reset(void)
{
    if (!s_started) {
        return;
    }
    ESP_LOGW(TAG, "[T+%07.3f] Matter factory reset solicitado", timebase_now_s());
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        for (size_t i = 0; i < DEVICE_TABLE_MAX_DEVICES; ++i) {
            if (s_slots[i].occupied) {
                remove_slot(&s_slots[i]);
            }
        }
        (void)esp_matter_bridge::factory_reset();
        (void)esp_matter::factory_reset();
    });
}
