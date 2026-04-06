#include "device_definition.h"
#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UNIT_DEG_C "\xC2\xB0""C"
#define ICON_SNOW "\xE2\x9D\x84\xEF\xB8\x8F"
#define ICON_FIRE "\xF0\x9F\x94\xA5"
#define ICON_SUN  "\xE2\x98\x80\xEF\xB8\x8F"
#define ICON_DROP "\xF0\x9F\x92\xA7"

typedef enum {
    DEF_KIND_NONE = 0,
    DEF_KIND_SNZB_02,
    DEF_KIND_SNZB_02D,
    DEF_KIND_RTCGQ01LM,
    DEF_KIND_ZBMINIL2,
    DEF_KIND_ZG_227Z_TUYA,
    DEF_KIND_ZG_227Z_HOBEIAN,
    DEF_KIND_ZG_227Z_KOJIMA,
    DEF_KIND_WHD02,
} definition_kind_t;

static void json_append(char **p, char *end, const char *fmt, ...)
{
    if (!p || !*p || *p >= end) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n >= end - *p) {
        *p = end;
        return;
    }
    *p += n;
}

static void json_append_string(char **p, char *end, const char *value)
{
    if (!value) value = "";

    json_append(p, end, "\"");
    while (*value && *p < end - 1) {
        unsigned char ch = (unsigned char)*value++;
        switch (ch) {
            case '\\':
                json_append(p, end, "\\\\");
                break;
            case '"':
                json_append(p, end, "\\\"");
                break;
            case '\n':
                json_append(p, end, "\\n");
                break;
            case '\r':
                json_append(p, end, "\\r");
                break;
            case '\t':
                json_append(p, end, "\\t");
                break;
            default:
                if (ch < 0x20) {
                    json_append(p, end, "\\u%04X", ch);
                } else {
                    *(*p)++ = (char)ch;
                    **p = '\0';
                }
                break;
        }
    }
    json_append(p, end, "\"");
}

static bool str_eq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static definition_kind_t definition_kind_for_device(const device_record_t *dev)
{
    if (!dev) return DEF_KIND_NONE;

    if (str_eq(dev->model, "SNZB-02D")) {
        return DEF_KIND_SNZB_02D;
    }

    if (str_eq(dev->model, "SNZB-02") ||
        str_eq(dev->model, "TH01") ||
        str_eq(dev->model, "CK-TLSR8656-SS5-01(7014)")) {
        return DEF_KIND_SNZB_02;
    }

    if (str_eq(dev->model, "RTCGQ01LM") ||
        str_eq(dev->model, "lumi.sensor_motion")) {
        return DEF_KIND_RTCGQ01LM;
    }

    if (str_eq(dev->model, "ZBMINIL2")) {
        return DEF_KIND_ZBMINIL2;
    }

    if (str_eq(dev->model, "TS0601") ||
        str_eq(dev->model, "ZG-227Z") ||
        str_eq(dev->model, "ZG-227ZL")) {
        if (str_eq(dev->manufacturer, "_TZE200_a8sdabtg") ||
            str_eq(dev->manufacturer, "_TZE200_vs0skpuc") ||
            str_eq(dev->manufacturer, "_TZE200_ehhrv2e3")) {
            return DEF_KIND_ZG_227Z_HOBEIAN;
        }
        if (str_eq(dev->manufacturer, "_TZE200_dikkika5")) {
            return DEF_KIND_ZG_227Z_KOJIMA;
        }
        return DEF_KIND_ZG_227Z_TUYA;
    }

    if (str_eq(dev->model, "TS0001") && str_eq(dev->manufacturer, "_TZ3000_fdxihpp7")) {
        return DEF_KIND_WHD02;
    }

    return DEF_KIND_NONE;
}

static void append_numeric_expose(char **p, char *end, const char *name,
                                  const char *label, const char *description,
                                  const char *unit, int access,
                                  const char *category,
                                  const char *extra_fields)
{
    json_append(p, end, "{");
    json_append(p, end, "\"access\":%d,", access);
    if (category) {
        json_append(p, end, "\"category\":");
        json_append_string(p, end, category);
        json_append(p, end, ",");
    }
    json_append(p, end, "\"description\":");
    json_append_string(p, end, description);
    json_append(p, end, ",\"label\":");
    json_append_string(p, end, label);
    json_append(p, end, ",\"name\":");
    json_append_string(p, end, name);
    json_append(p, end, ",\"property\":");
    json_append_string(p, end, name);
    json_append(p, end, ",\"type\":\"numeric\"");
    if (unit) {
        json_append(p, end, ",\"unit\":");
        json_append_string(p, end, unit);
    }
    if (extra_fields) {
        json_append(p, end, ",%s", extra_fields);
    }
    json_append(p, end, "}");
}

static void append_binary_expose(char **p, char *end, const char *name,
                                 const char *label, const char *description,
                                 int access, const char *category,
                                 const char *value_on, const char *value_off,
                                 bool string_values, const char *extra_fields)
{
    json_append(p, end, "{");
    json_append(p, end, "\"access\":%d,", access);
    if (category) {
        json_append(p, end, "\"category\":");
        json_append_string(p, end, category);
        json_append(p, end, ",");
    }
    json_append(p, end, "\"description\":");
    json_append_string(p, end, description);
    json_append(p, end, ",\"label\":");
    json_append_string(p, end, label);
    json_append(p, end, ",\"name\":");
    json_append_string(p, end, name);
    json_append(p, end, ",\"property\":");
    json_append_string(p, end, name);
    json_append(p, end, ",\"type\":\"binary\",");
    json_append(p, end, "\"value_off\":");
    if (string_values) {
        json_append_string(p, end, value_off);
    } else {
        json_append(p, end, "%s", value_off);
    }
    json_append(p, end, ",\"value_on\":");
    if (string_values) {
        json_append_string(p, end, value_on);
    } else {
        json_append(p, end, "%s", value_on);
    }
    if (extra_fields) {
        json_append(p, end, ",%s", extra_fields);
    }
    json_append(p, end, "}");
}

static void append_option_numeric(char **p, char *end, const char *name,
                                  const char *label, const char *description,
                                  const char *extra_fields)
{
    append_numeric_expose(p, end, name, label, description, NULL, 2, NULL, extra_fields);
}

static void append_option_binary(char **p, char *end, const char *name,
                                 const char *label, const char *description)
{
    append_binary_expose(p, end, name, label, description, 2, NULL,
                         "true", "false", false, NULL);
}

static void append_linkquality_expose(char **p, char *end)
{
    append_numeric_expose(
        p, end, "linkquality", "Linkquality", "Link quality (signal strength)",
        "lqi", 1, "diagnostic", "\"value_max\":255,\"value_min\":0");
}

static void append_battery_expose(char **p, char *end, int access, const char *description)
{
    append_numeric_expose(
        p, end, "battery", "Battery", description,
        "%", access, "diagnostic", "\"value_max\":100,\"value_min\":0");
}

static void append_voltage_expose(char **p, char *end)
{
    append_numeric_expose(
        p, end, "voltage", "Voltage",
        "Voltage of the battery in millivolts", "mV", 1, "diagnostic", NULL);
}

static void append_temperature_expose(char **p, char *end, int access)
{
    append_numeric_expose(
        p, end, "temperature", "Temperature",
        "Measured temperature value", UNIT_DEG_C, access, NULL, NULL);
}

static void append_humidity_expose(char **p, char *end, int access)
{
    append_numeric_expose(
        p, end, "humidity", "Humidity",
        "Measured relative humidity", "%", access, NULL, NULL);
}

static void append_occupancy_expose(char **p, char *end)
{
    append_binary_expose(
        p, end, "occupancy", "Occupancy",
        "Indicates whether the device detected occupancy",
        1, NULL, "true", "false", false, NULL);
}

static void append_switch_expose(char **p, char *end)
{
    json_append(p, end, "{");
    json_append(p, end, "\"features\":[");
    append_binary_expose(
        p, end, "state", "State", "On/off state of the switch",
        7, NULL, "ON", "OFF", true, "\"value_toggle\":\"TOGGLE\"");
    json_append(p, end, "],\"type\":\"switch\"}");
}

static void append_light_expose(char **p, char *end, bool has_brightness)
{
    json_append(p, end, "{");
    json_append(p, end, "\"features\":[");
    append_binary_expose(
        p, end, "state", "State", "On/off state of the light",
        7, NULL, "ON", "OFF", true, "\"value_toggle\":\"TOGGLE\"");

    if (has_brightness) {
        json_append(p, end, ",");
        append_numeric_expose(
            p, end, "brightness", "Brightness",
            "Brightness of the light", NULL, 7, NULL,
            "\"value_max\":254,\"value_min\":0");
    }

    json_append(p, end, "],\"type\":\"light\"}");
}

static void append_pressure_expose(char **p, char *end, int access)
{
    append_numeric_expose(
        p, end, "pressure", "Pressure",
        "Measured pressure value", "hPa", access, NULL, NULL);
}

static void append_illuminance_expose(char **p, char *end, int access)
{
    append_numeric_expose(
        p, end, "illuminance", "Illuminance",
        "Measured illuminance value", "lx", access, NULL, NULL);
}

static void append_power_expose(char **p, char *end, int access)
{
    append_numeric_expose(
        p, end, "power", "Power",
        "Measured instantaneous power", "W", access, NULL, NULL);
}

static bool device_has_input_cluster(const device_record_t *dev, uint16_t cluster_id)
{
    return dm_has_in_cluster(dev, cluster_id, NULL);
}

static bool endpoint_has_out_cluster(const endpoint_record_t *ep, uint16_t cluster_id)
{
    if (!ep) return false;

    for (uint8_t i = 0; i < ep->out_cluster_count; i++) {
        if (ep->out_clusters[i] == cluster_id) {
            return true;
        }
    }

    return false;
}

static bool device_has_output_cluster(const device_record_t *dev, uint16_t cluster_id)
{
    if (!dev) return false;

    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        if (endpoint_has_out_cluster(&dev->endpoints[i], cluster_id)) {
            return true;
        }
    }

    return false;
}

static bool device_has_device_id(const device_record_t *dev, uint16_t device_id)
{
    if (!dev) return false;

    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        if (dev->endpoints[i].endpoint_id != 0 &&
            dev->endpoints[i].device_id == device_id) {
            return true;
        }
    }

    return false;
}

static int read_access_for_device(const device_record_t *dev)
{
    return (dev && !dev->is_sleepy) ? 5 : 1;
}

static bool device_supports_ota(const device_record_t *dev)
{
    return device_has_output_cluster(dev, 0x0019u);
}

static bool device_is_light(const device_record_t *dev)
{
    if (!dev) return false;

    if (device_has_input_cluster(dev, 0x0300u)) {
        return true;
    }

    return device_has_device_id(dev, 0x0100u) ||
           device_has_device_id(dev, 0x0101u) ||
           device_has_device_id(dev, 0x0102u);
}

static const char *dynamic_definition_description(const device_record_t *dev)
{
    if (device_is_light(dev)) {
        return "Light";
    }
    if (device_has_input_cluster(dev, 0x0006u)) {
        return "Switch";
    }
    if (device_has_input_cluster(dev, 0x0402u) &&
        device_has_input_cluster(dev, 0x0405u)) {
        return "Temperature and humidity sensor";
    }
    if (device_has_input_cluster(dev, 0x0406u)) {
        return "Occupancy sensor";
    }
    if (device_has_input_cluster(dev, 0x0402u)) {
        return "Temperature sensor";
    }
    if (device_has_input_cluster(dev, 0x0405u)) {
        return "Humidity sensor";
    }
    if (device_has_input_cluster(dev, 0x0400u)) {
        return "Illuminance sensor";
    }
    if (device_has_input_cluster(dev, 0x0500u)) {
        return "IAS zone sensor";
    }
    return "Zigbee device";
}

static const char *dynamic_definition_model(const device_record_t *dev)
{
    if (!dev) return "Unknown";
    if (dev->model[0]) return dev->model;

    for (uint8_t i = 0; i < dev->endpoint_count; i++) {
        if (dev->endpoints[i].endpoint_id != 0) {
            return utils_device_type_name(dev->endpoints[i].device_id);
        }
    }

    return "Unknown";
}

static const char *dynamic_definition_vendor(const device_record_t *dev)
{
    if (dev && dev->manufacturer[0]) {
        return dev->manufacturer;
    }
    return "Unknown";
}

static void append_no_occupancy_since_option(char **p, char *end)
{
    json_append(p, end, "{");
    json_append(p, end, "\"access\":2,");
    json_append(p, end, "\"description\":");
    json_append_string(
        p, end,
        "Sends a message the last time occupancy (occupancy: true) was detected. "
        "When setting this for example to [10, 60] a `{\"no_occupancy_since\": 10}` "
        "will be sent after 10 seconds and a `{\"no_occupancy_since\": 60}` after 60 seconds.");
    json_append(p, end, ",\"item_type\":{\"access\":3,\"label\":\"Time\",\"name\":\"time\",\"type\":\"numeric\"}");
    json_append(p, end, ",\"label\":\"No occupancy since\",\"name\":\"no_occupancy_since\"");
    json_append(p, end, ",\"property\":\"no_occupancy_since\",\"type\":\"list\"}");
}

static void append_definition_fields(char **p, char *end, definition_kind_t kind)
{
    const char *description = NULL;
    const char *model = NULL;
    const char *vendor = NULL;
    bool supports_ota = false;

    switch (kind) {
        case DEF_KIND_SNZB_02:
            description = "Temperature and humidity sensor";
            model = "SNZB-02";
            vendor = "SONOFF";
            break;
        case DEF_KIND_SNZB_02D:
            description = "Temperature and humidity sensor with screen";
            model = "SNZB-02D";
            vendor = "SONOFF";
            supports_ota = true;
            break;
        case DEF_KIND_RTCGQ01LM:
            description = "Mi motion sensor";
            model = "RTCGQ01LM";
            vendor = "Xiaomi";
            break;
        case DEF_KIND_ZBMINIL2:
            description = "Zigbee smart switch (no neutral)";
            model = "ZBMINIL2";
            vendor = "SONOFF";
            supports_ota = true;
            break;
        case DEF_KIND_ZG_227Z_TUYA:
            description = "Temperature & humidity LCD sensor";
            model = "ZG-227ZL";
            vendor = "Tuya";
            break;
        case DEF_KIND_ZG_227Z_HOBEIAN:
            description = "Temperature and humidity sensor";
            model = "ZG-227Z";
            vendor = "HOBEIAN";
            break;
        case DEF_KIND_ZG_227Z_KOJIMA:
            description = "Temperature and humidity sensor";
            model = "KOJIMA-THS-ZG-LCD";
            vendor = "KOJIMA";
            break;
        case DEF_KIND_WHD02:
            description = "Wall switch module";
            model = "WHD02";
            vendor = "Tuya";
            break;
        default:
            return;
    }

    json_append(p, end, "\"description\":");
    json_append_string(p, end, description);
    json_append(p, end, ",\"model\":");
    json_append_string(p, end, model);
    json_append(p, end, ",\"source\":\"native\",\"supports_ota\":%s,\"vendor\":",
                supports_ota ? "true" : "false");
    json_append_string(p, end, vendor);
}

static void append_dynamic_definition_fields(char **p, char *end,
                                             const device_record_t *dev)
{
    json_append(p, end, "\"description\":");
    json_append_string(p, end, dynamic_definition_description(dev));
    json_append(p, end, ",\"model\":");
    json_append_string(p, end, dynamic_definition_model(dev));
    json_append(p, end, ",\"source\":\"interview\",\"supports_ota\":%s,\"vendor\":",
                device_supports_ota(dev) ? "true" : "false");
    json_append_string(p, end, dynamic_definition_vendor(dev));
}

static void append_temp_humidity_options(char **p, char *end)
{
    append_option_numeric(
        p, end, "temperature_calibration", "Temperature calibration",
        "Calibrates the temperature value (absolute offset), takes into effect on next report of device.",
        "\"value_step\":0.1");
    json_append(p, end, ",");
    append_option_numeric(
        p, end, "temperature_precision", "Temperature precision",
        "Number of digits after decimal point for temperature, takes into effect on next report of device. "
        "This option can only decrease the precision, not increase it.",
        "\"value_max\":3,\"value_min\":0");
    json_append(p, end, ",");
    append_option_numeric(
        p, end, "humidity_calibration", "Humidity calibration",
        "Calibrates the humidity value (absolute offset), takes into effect on next report of device.",
        "\"value_step\":0.1");
    json_append(p, end, ",");
    append_option_numeric(
        p, end, "humidity_precision", "Humidity precision",
        "Number of digits after decimal point for humidity, takes into effect on next report of device. "
        "This option can only decrease the precision, not increase it.",
        "\"value_max\":3,\"value_min\":0");
}

static void append_exposes_json(char **p, char *end, const device_record_t *dev)
{
    bool first = true;
    int read_access = read_access_for_device(dev);

#define APPEND_EXPOSE(call)            \
    do {                               \
        if (!first) {                  \
            json_append(p, end, ",");  \
        }                              \
        first = false;                 \
        call;                          \
    } while (0)

    json_append(p, end, "\"exposes\":[");

    if (device_is_light(dev)) {
        APPEND_EXPOSE(append_light_expose(
            p, end, device_has_input_cluster(dev, 0x0008u)));
    } else if (device_has_input_cluster(dev, 0x0006u)) {
        APPEND_EXPOSE(append_switch_expose(p, end));
    }

    if (device_has_input_cluster(dev, 0x0402u)) {
        APPEND_EXPOSE(append_temperature_expose(p, end, read_access));
    }

    if (device_has_input_cluster(dev, 0x0405u)) {
        APPEND_EXPOSE(append_humidity_expose(p, end, read_access));
    }

    if (device_has_input_cluster(dev, 0x0403u)) {
        APPEND_EXPOSE(append_pressure_expose(p, end, read_access));
    }

    if (device_has_input_cluster(dev, 0x0400u)) {
        APPEND_EXPOSE(append_illuminance_expose(p, end, read_access));
    }

    if (device_has_input_cluster(dev, 0x0406u)) {
        APPEND_EXPOSE(append_occupancy_expose(p, end));
    }

    if (device_has_input_cluster(dev, 0x0001u)) {
        APPEND_EXPOSE(append_battery_expose(
            p, end, read_access, "Remaining battery in %"));
        APPEND_EXPOSE(append_voltage_expose(p, end));
    }

    if (device_has_input_cluster(dev, 0x0B04u)) {
        APPEND_EXPOSE(append_power_expose(p, end, read_access));
    }

    APPEND_EXPOSE(append_linkquality_expose(p, end));

    json_append(p, end, "]");

#undef APPEND_EXPOSE
}

static void append_options_json(char **p, char *end, definition_kind_t kind)
{
    json_append(p, end, "\"options\":[");

    switch (kind) {
        case DEF_KIND_SNZB_02:
        case DEF_KIND_SNZB_02D:
        case DEF_KIND_ZG_227Z_TUYA:
        case DEF_KIND_ZG_227Z_HOBEIAN:
        case DEF_KIND_ZG_227Z_KOJIMA:
            append_temp_humidity_options(p, end);
            break;

        case DEF_KIND_RTCGQ01LM:
            append_option_numeric(
                p, end, "occupancy_timeout", "Occupancy timeout",
                "Time in seconds after which occupancy is cleared after detecting it (default 90 seconds).",
                "\"value_min\":0");
            json_append(p, end, ",");
            append_no_occupancy_since_option(p, end);
            break;

        case DEF_KIND_ZBMINIL2:
        case DEF_KIND_WHD02:
            append_option_binary(
                p, end, "state_action", "State action",
                "State actions will also be published as 'action' when true (default false).");
            break;

        default:
            break;
    }

    json_append(p, end, "]");
}

bool dd_has_definition(const device_record_t *dev)
{
    if (!dev) return false;

    return definition_kind_for_device(dev) != DEF_KIND_NONE ||
           dev->endpoint_count > 0 ||
           dev->model[0] != '\0' ||
           dev->manufacturer[0] != '\0';
}

bool dd_is_supported(const device_record_t *dev)
{
    return dd_has_definition(dev);
}

void dd_append_definition_json(char **p, char *end, const device_record_t *dev)
{
    definition_kind_t kind = definition_kind_for_device(dev);
    if (!dd_has_definition(dev)) return;

    json_append(p, end, ",\"definition\":{");
    if (kind != DEF_KIND_NONE) {
        append_definition_fields(p, end, kind);
    } else {
        append_dynamic_definition_fields(p, end, dev);
    }
    json_append(p, end, ",");
    append_exposes_json(p, end, dev);
    json_append(p, end, ",");
    append_options_json(p, end, kind);
    json_append(p, end, "}");
}
