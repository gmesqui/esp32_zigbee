#include "app_config.h"

#include "nvs.h"
#include "utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <string.h>

#define APP_CONFIG_NAMESPACE "app_cfg"
#define APP_CONFIG_KEY_MDNS_HOSTNAME "mdns_host"
#define APP_CONFIG_KEY_MDNS_INSTANCE "mdns_inst"
#define APP_CONFIG_KEY_JOIN_SECS "join_secs"
#define APP_CONFIG_KEY_NTP_SERVER "ntp_srv"
#define APP_CONFIG_KEY_TIMEZONE "tz"
#define APP_CONFIG_KEY_REPORT_AON "rep_aon"
#define APP_CONFIG_KEY_REPORT_SLEEPY "rep_slp"
#define APP_CONFIG_KEY_PRES_GRACE "pres_gr"
#define APP_CONFIG_KEY_PRES_PROBE_GRACE "pres_probe"
#define APP_CONFIG_KEY_PRES_OFFLINE_GRACE "pres_off"

#define APP_CONFIG_PERMIT_JOIN_MIN_S 10u
#define APP_CONFIG_PERMIT_JOIN_MAX_S 254u
#define APP_CONFIG_REPORT_ALWAYS_ON_MIN_S 30u
#define APP_CONFIG_REPORT_ALWAYS_ON_MAX_S 3600u
#define APP_CONFIG_REPORT_SLEEPY_MIN_S 300u
#define APP_CONFIG_REPORT_SLEEPY_MAX_S 43200u
#define APP_CONFIG_PRESENCE_PROBE_GRACE_MIN_S 5u
#define APP_CONFIG_PRESENCE_PROBE_GRACE_MAX_S 3600u
#define APP_CONFIG_PRESENCE_OFFLINE_GRACE_MIN_S 10u
#define APP_CONFIG_PRESENCE_OFFLINE_GRACE_MAX_S 7200u
#define APP_CONFIG_PRESENCE_OFFLINE_EXTRA_MIN_S 10u

static app_config_t s_config;
static SemaphoreHandle_t s_config_lock;

void app_config_defaults(app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->mdns_hostname, APP_CONFIG_DEFAULT_MDNS_HOSTNAME,
            sizeof(cfg->mdns_hostname) - 1);
    strncpy(cfg->mdns_instance, APP_CONFIG_DEFAULT_MDNS_INSTANCE,
            sizeof(cfg->mdns_instance) - 1);
    strncpy(cfg->ntp_server, APP_CONFIG_DEFAULT_NTP_SERVER,
            sizeof(cfg->ntp_server) - 1);
    strncpy(cfg->timezone, APP_CONFIG_DEFAULT_TIMEZONE,
            sizeof(cfg->timezone) - 1);
    cfg->permit_join_duration_s = APP_CONFIG_DEFAULT_PERMIT_JOIN_DURATION_S;
    cfg->report_always_on_max_s = APP_CONFIG_DEFAULT_REPORT_ALWAYS_ON_MAX_S;
    cfg->report_sleepy_max_s = APP_CONFIG_DEFAULT_REPORT_SLEEPY_MAX_S;
    cfg->presence_probe_grace_s = APP_CONFIG_DEFAULT_PRESENCE_PROBE_GRACE_S;
    cfg->presence_offline_grace_s = APP_CONFIG_DEFAULT_PRESENCE_OFFLINE_GRACE_S;
}

bool app_config_hostname_is_valid(const char *hostname)
{
    if (!hostname || hostname[0] == '\0') {
        return false;
    }

    size_t len = strnlen(hostname, APP_CONFIG_MDNS_HOSTNAME_LEN);
    if (len == 0 || len >= APP_CONFIG_MDNS_HOSTNAME_LEN) {
        return false;
    }
    if (hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)hostname[i];
        if (!(isalnum(ch) || ch == '-')) {
            return false;
        }
    }
    return true;
}

uint16_t app_config_clamp_permit_join_duration(uint32_t duration_s)
{
    if (duration_s < APP_CONFIG_PERMIT_JOIN_MIN_S) {
        return APP_CONFIG_PERMIT_JOIN_MIN_S;
    }
    if (duration_s > APP_CONFIG_PERMIT_JOIN_MAX_S) {
        return APP_CONFIG_PERMIT_JOIN_MAX_S;
    }
    return (uint16_t)duration_s;
}

uint16_t app_config_clamp_report_always_on_max(uint32_t seconds)
{
    if (seconds < APP_CONFIG_REPORT_ALWAYS_ON_MIN_S) {
        return APP_CONFIG_REPORT_ALWAYS_ON_MIN_S;
    }
    if (seconds > APP_CONFIG_REPORT_ALWAYS_ON_MAX_S) {
        return APP_CONFIG_REPORT_ALWAYS_ON_MAX_S;
    }
    return (uint16_t)seconds;
}

uint16_t app_config_clamp_report_sleepy_max(uint32_t seconds)
{
    if (seconds < APP_CONFIG_REPORT_SLEEPY_MIN_S) {
        return APP_CONFIG_REPORT_SLEEPY_MIN_S;
    }
    if (seconds > APP_CONFIG_REPORT_SLEEPY_MAX_S) {
        return APP_CONFIG_REPORT_SLEEPY_MAX_S;
    }
    return (uint16_t)seconds;
}

uint16_t app_config_clamp_presence_probe_grace(uint32_t seconds)
{
    if (seconds < APP_CONFIG_PRESENCE_PROBE_GRACE_MIN_S) {
        return APP_CONFIG_PRESENCE_PROBE_GRACE_MIN_S;
    }
    if (seconds > APP_CONFIG_PRESENCE_PROBE_GRACE_MAX_S) {
        return APP_CONFIG_PRESENCE_PROBE_GRACE_MAX_S;
    }
    return (uint16_t)seconds;
}

uint16_t app_config_clamp_presence_offline_grace(uint32_t seconds)
{
    if (seconds < APP_CONFIG_PRESENCE_OFFLINE_GRACE_MIN_S) {
        return APP_CONFIG_PRESENCE_OFFLINE_GRACE_MIN_S;
    }
    if (seconds > APP_CONFIG_PRESENCE_OFFLINE_GRACE_MAX_S) {
        return APP_CONFIG_PRESENCE_OFFLINE_GRACE_MAX_S;
    }
    return (uint16_t)seconds;
}

static void normalize_presence_graces(app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->presence_probe_grace_s =
        app_config_clamp_presence_probe_grace(cfg->presence_probe_grace_s);
    cfg->presence_offline_grace_s =
        app_config_clamp_presence_offline_grace(cfg->presence_offline_grace_s);

    uint32_t min_offline =
        (uint32_t)cfg->presence_probe_grace_s + APP_CONFIG_PRESENCE_OFFLINE_EXTRA_MIN_S;
    if (cfg->presence_offline_grace_s < min_offline) {
        cfg->presence_offline_grace_s =
            app_config_clamp_presence_offline_grace(min_offline);
    }
}

static void load_str(nvs_handle_t handle, const char *key, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }

    size_t len = dst_len;
    esp_err_t err = nvs_get_str(handle, key, dst, &len);
    if (err != ESP_OK) {
        return;
    }
    dst[dst_len - 1] = '\0';
}

void app_config_init(void)
{
    app_config_defaults(&s_config);
    s_config_lock = xSemaphoreCreateMutex();
    configASSERT(s_config_lock);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ZB_LOG("APP_CONFIG defaults active");
        return;
    }

    app_config_t loaded = s_config;
    load_str(handle, APP_CONFIG_KEY_MDNS_HOSTNAME,
             loaded.mdns_hostname, sizeof(loaded.mdns_hostname));
    load_str(handle, APP_CONFIG_KEY_MDNS_INSTANCE,
             loaded.mdns_instance, sizeof(loaded.mdns_instance));
    load_str(handle, APP_CONFIG_KEY_NTP_SERVER,
             loaded.ntp_server, sizeof(loaded.ntp_server));
    load_str(handle, APP_CONFIG_KEY_TIMEZONE,
             loaded.timezone, sizeof(loaded.timezone));

    uint16_t join_secs = 0;
    if (nvs_get_u16(handle, APP_CONFIG_KEY_JOIN_SECS, &join_secs) == ESP_OK) {
        loaded.permit_join_duration_s =
            app_config_clamp_permit_join_duration(join_secs);
    }
    uint16_t report_secs = 0;
    if (nvs_get_u16(handle, APP_CONFIG_KEY_REPORT_AON, &report_secs) == ESP_OK) {
        loaded.report_always_on_max_s =
            app_config_clamp_report_always_on_max(report_secs);
    }
    if (nvs_get_u16(handle, APP_CONFIG_KEY_REPORT_SLEEPY, &report_secs) == ESP_OK) {
        loaded.report_sleepy_max_s =
            app_config_clamp_report_sleepy_max(report_secs);
    }
    if (nvs_get_u16(handle, APP_CONFIG_KEY_PRES_GRACE, &report_secs) == ESP_OK) {
        loaded.presence_probe_grace_s =
            app_config_clamp_presence_probe_grace(report_secs);
        loaded.presence_offline_grace_s =
            app_config_clamp_presence_offline_grace(
                (uint32_t)loaded.presence_probe_grace_s +
                APP_CONFIG_PRESENCE_OFFLINE_EXTRA_MIN_S * 2u);
    }
    if (nvs_get_u16(handle, APP_CONFIG_KEY_PRES_PROBE_GRACE, &report_secs) == ESP_OK) {
        loaded.presence_probe_grace_s =
            app_config_clamp_presence_probe_grace(report_secs);
    }
    if (nvs_get_u16(handle, APP_CONFIG_KEY_PRES_OFFLINE_GRACE, &report_secs) == ESP_OK) {
        loaded.presence_offline_grace_s =
            app_config_clamp_presence_offline_grace(report_secs);
    }
    nvs_close(handle);
    normalize_presence_graces(&loaded);

    if (!app_config_hostname_is_valid(loaded.mdns_hostname)) {
        strncpy(loaded.mdns_hostname, APP_CONFIG_DEFAULT_MDNS_HOSTNAME,
                sizeof(loaded.mdns_hostname) - 1);
        loaded.mdns_hostname[sizeof(loaded.mdns_hostname) - 1] = '\0';
    }
    if (loaded.mdns_instance[0] == '\0') {
        strncpy(loaded.mdns_instance, APP_CONFIG_DEFAULT_MDNS_INSTANCE,
                sizeof(loaded.mdns_instance) - 1);
        loaded.mdns_instance[sizeof(loaded.mdns_instance) - 1] = '\0';
    }
    if (loaded.ntp_server[0] == '\0') {
        strncpy(loaded.ntp_server, APP_CONFIG_DEFAULT_NTP_SERVER,
                sizeof(loaded.ntp_server) - 1);
        loaded.ntp_server[sizeof(loaded.ntp_server) - 1] = '\0';
    }
    if (loaded.timezone[0] == '\0') {
        strncpy(loaded.timezone, APP_CONFIG_DEFAULT_TIMEZONE,
                sizeof(loaded.timezone) - 1);
        loaded.timezone[sizeof(loaded.timezone) - 1] = '\0';
    }

    if (xSemaphoreTake(s_config_lock, portMAX_DELAY) == pdTRUE) {
        s_config = loaded;
        xSemaphoreGive(s_config_lock);
    }

    ZB_LOG("APP_CONFIG hostname=%s.local join_default=%u",
           s_config.mdns_hostname, s_config.permit_join_duration_s);
}

void app_config_get(app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (s_config_lock && xSemaphoreTake(s_config_lock, portMAX_DELAY) == pdTRUE) {
        *cfg = s_config;
        xSemaphoreGive(s_config_lock);
        return;
    }

    *cfg = s_config;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg || !app_config_hostname_is_valid(cfg->mdns_hostname) ||
        cfg->mdns_instance[0] == '\0' || cfg->ntp_server[0] == '\0' ||
        cfg->timezone[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t next = *cfg;
    next.mdns_hostname[sizeof(next.mdns_hostname) - 1] = '\0';
    next.mdns_instance[sizeof(next.mdns_instance) - 1] = '\0';
    next.ntp_server[sizeof(next.ntp_server) - 1] = '\0';
    next.timezone[sizeof(next.timezone) - 1] = '\0';
    next.permit_join_duration_s =
        app_config_clamp_permit_join_duration(next.permit_join_duration_s);
    next.report_always_on_max_s =
        app_config_clamp_report_always_on_max(next.report_always_on_max_s);
    next.report_sleepy_max_s =
        app_config_clamp_report_sleepy_max(next.report_sleepy_max_s);
    normalize_presence_graces(&next);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, APP_CONFIG_KEY_MDNS_HOSTNAME,
                      next.mdns_hostname);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, APP_CONFIG_KEY_MDNS_INSTANCE,
                          next.mdns_instance);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_JOIN_SECS,
                          next.permit_join_duration_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, APP_CONFIG_KEY_NTP_SERVER,
                          next.ntp_server);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, APP_CONFIG_KEY_TIMEZONE,
                          next.timezone);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_REPORT_AON,
                          next.report_always_on_max_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_REPORT_SLEEPY,
                          next.report_sleepy_max_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_PRES_GRACE,
                          next.presence_probe_grace_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_PRES_PROBE_GRACE,
                          next.presence_probe_grace_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, APP_CONFIG_KEY_PRES_OFFLINE_GRACE,
                          next.presence_offline_grace_s);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (s_config_lock && xSemaphoreTake(s_config_lock, portMAX_DELAY) == pdTRUE) {
        s_config = next;
        xSemaphoreGive(s_config_lock);
    } else {
        s_config = next;
    }

    ZB_LOG("APP_CONFIG saved hostname=%s.local join_default=%u",
           s_config.mdns_hostname, s_config.permit_join_duration_s);
    return ESP_OK;
}
