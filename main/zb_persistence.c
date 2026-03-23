#include "zb_persistence.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "zb_persist";
static const char *NAMESPACE = "zb_state";
static const char *KEY_BLOB = "state";

esp_err_t zb_persistence_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t zb_persistence_load(zb_persist_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->version = ZB_PERSIST_VERSION;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out_state);
    err = nvs_get_blob(nvs, KEY_BLOB, out_state, &len);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out_state, 0, sizeof(*out_state));
        out_state->version = ZB_PERSIST_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(*out_state) || out_state->version != ZB_PERSIST_VERSION) {
        ESP_LOGW(TAG, "Estado NVS incompatible, usando factory-new");
        memset(out_state, 0, sizeof(*out_state));
        out_state->version = ZB_PERSIST_VERSION;
    }
    return ESP_OK;
}

esp_err_t zb_persistence_save(const zb_persist_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, KEY_BLOB, state, sizeof(*state));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t zb_persistence_clear(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(nvs, KEY_BLOB);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
