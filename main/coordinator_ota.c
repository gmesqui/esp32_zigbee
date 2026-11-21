#include "coordinator_ota.h"
#include "device_manager.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_cache.h"
#include "zigbee_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Coordinator OTA requires bootloader application rollback"
#endif

#define NETWORK_RESTORED BIT0
#define SERVER_READY BIT1
#define SHA_BYTES 32
#define BLOCK_SIZE 4096
#define AUTH_DOMAIN "esp32_zigbee3/upload/v1"
static const char *TAG = "coordinator_ota";
extern const uint8_t public_key[] asm("_binary_ota_public_pem_start");
static EventGroupHandle_t health;
// Set once before starting either the network server or the Zigbee stack.
static bool trial;
static uint8_t challenge[SHA_BYTES];

typedef struct {
    uint8_t extended_pan[8];
    uint16_t pan;
    uint8_t channel;
    uint8_t count;
    uint64_t devices[MAX_DEVICES];
} network_id_t;

static network_id_t network_id(void)
{
    network_id_t id = { .pan = esp_zb_get_pan_id(), .channel = esp_zb_get_current_channel() };
    esp_zb_get_extended_pan_id(id.extended_pan);
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        const device_record_t *dev = dm_get_by_index(i);
        if (dev) id.devices[id.count++] = dev->ieee_addr;
    }
    return id;
}

bool coordinator_ota_trial_boot(void)
{
    return trial;
}

bool coordinator_ota_network_matches(void)
{
    if (!trial) return true;
    network_id_t expected;
    size_t len = sizeof(expected);
    nvs_handle_t db;
    if (nvs_open("fw_ota", NVS_READONLY, &db) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(db, "network", &expected, &len);
    nvs_close(db);
    uint8_t current_pan[8];
    esp_zb_get_extended_pan_id(current_pan);
    if (err != ESP_OK || len != sizeof(expected) || expected.count > MAX_DEVICES ||
        expected.pan != esp_zb_get_pan_id() || expected.channel != esp_zb_get_current_channel() ||
        memcmp(expected.extended_pan, current_pan, sizeof(current_pan)) != 0) return false;
    for (uint8_t i = 0; i < expected.count; i++) {
        if (!dm_find_by_ieee(expected.devices[i])) return false;
    }
    xEventGroupSetBits(health, NETWORK_RESTORED);
    return true;
}

static void check_boot(void *arg)
{
    const EventBits_t required = NETWORK_RESTORED | SERVER_READY;
    EventBits_t bits = xEventGroupWaitBits(health, required, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(120000));
    if ((bits & required) == required && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "OTA confirmed: original Zigbee network and Ethernet services ready");
    } else {
        ESP_LOGE(TAG, "OTA health check failed, rolling back");
        ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
    }
    vTaskDelete(NULL);
}

void coordinator_ota_init(void)
{
    esp_ota_img_states_t state;
    ESP_ERROR_CHECK(esp_ota_get_state_partition(esp_ota_get_running_partition(), &state));
    trial = state == ESP_OTA_IMG_PENDING_VERIFY;
    if (trial) {
        health = xEventGroupCreate();
        ESP_ERROR_CHECK(health ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(check_boot, "ota_health", 4096, NULL, 4, NULL)
                        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
}

static esp_err_t reject(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, message);
    return ESP_FAIL; // Close even if an unread request body remains.
}

static esp_err_t info(httpd_req_t *req)
{
    char nonce[2 * SHA_BYTES + 1], elf[65], json[480];
    const esp_app_desc_t *app = esp_app_get_description();
    for (size_t i = 0; i < sizeof(challenge); i++) {
        snprintf(nonce + 2*i, 3, "%02x", challenge[i]);
        snprintf(elf + 2*i, 3, "%02x", app->app_elf_sha256[i]);
    }
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    bool valid = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID;
    cJSON *obj = cJSON_CreateObject();
    if (!obj || !cJSON_AddStringToObject(obj, "product", app->project_name) ||
        !cJSON_AddStringToObject(obj, "version", app->version) ||
        !cJSON_AddStringToObject(obj, "chip", CONFIG_IDF_TARGET) ||
        !cJSON_AddStringToObject(obj, "elf_sha256", elf) ||
        !cJSON_AddStringToObject(obj, "slot", running->label) ||
        !cJSON_AddBoolToObject(obj, "confirmed", valid) ||
        !cJSON_AddStringToObject(obj, "nonce", nonce) ||
        !cJSON_PrintPreallocated(obj, json, sizeof(json), false)) {
        cJSON_Delete(obj);
        return reject(req, "500 Internal Server Error", "Cannot encode OTA status");
    }
    cJSON_Delete(obj);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static bool read_header(httpd_req_t *req, const char *name, uint8_t *out,
                        size_t capacity, size_t *length)
{
    char encoded[104];
    return httpd_req_get_hdr_value_str(req, name, encoded, sizeof(encoded)) == ESP_OK &&
           mbedtls_base64_decode(out, capacity, length, (uint8_t *)encoded,
                                 strlen(encoded)) == 0 && *length != 0;
}

static bool authorized(httpd_req_t *req, uint8_t digest[SHA_BYTES])
{
    size_t n = 0, release_len = 0, auth_len = 0;
    uint8_t release[72], auth[72], signed_request[sizeof(AUTH_DOMAIN)-1 + 2 * SHA_BYTES];
    uint8_t auth_hash[SHA_BYTES];
    if (!read_header(req, "X-Image-SHA256", digest, SHA_BYTES, &n) || n != SHA_BYTES ||
        !read_header(req, "X-Image-Signature", release, sizeof(release), &release_len) ||
        !read_header(req, "X-Upload-Signature", auth, sizeof(auth), &auth_len)) return false;
    memcpy(signed_request, AUTH_DOMAIN, sizeof(AUTH_DOMAIN)-1);
    memcpy(signed_request + sizeof(AUTH_DOMAIN)-1, challenge, SHA_BYTES);
    memcpy(signed_request + sizeof(AUTH_DOMAIN)-1 + SHA_BYTES, digest, SHA_BYTES);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = mbedtls_sha256(signed_request, sizeof(signed_request), auth_hash, 0) == 0 &&
              mbedtls_pk_parse_public_key(&key, public_key, strlen((const char *)public_key)+1) == 0 &&
              mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, auth_hash, SHA_BYTES, auth, auth_len) == 0 &&
              mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, SHA_BYTES, release, release_len) == 0;
    mbedtls_pk_free(&key);
    return ok;
}

static bool is_coordinator_image(const uint8_t *block)
{
    esp_image_header_t header;
    esp_app_desc_t desc;
    memcpy(&header, block, sizeof(header));
    memcpy(&desc, block + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(desc));
    return header.magic == ESP_IMAGE_HEADER_MAGIC && header.chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID &&
           desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           memcmp(desc.project_name, "esp32_zigbee3", sizeof("esp32_zigbee3")) == 0;
}

static esp_err_t remember_network(void)
{
    if (!zigbee_core_is_ready() || !esp_zb_lock_acquire(pdMS_TO_TICKS(1500))) return ESP_ERR_INVALID_STATE;
    network_id_t id = network_id();
    bool saved = nvs_cache_save_dirty();
    esp_zb_lock_release();
    if (!saved) return ESP_FAIL;
    nvs_handle_t db;
    esp_err_t err = nvs_open("fw_ota", NVS_READWRITE, &db);
    if (err == ESP_OK) {
        err = nvs_set_blob(db, "network", &id, sizeof(id));
        if (err == ESP_OK) err = nvs_commit(db);
        nvs_close(db);
    }
    return err;
}

static esp_err_t upload(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;
    uint8_t expected[SHA_BYTES];
    if (!next || next == running || !zigbee_core_is_ready() ||
        esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID)
        return reject(req, "409 Conflict", "Coordinator not ready");
    if (req->content_len < BLOCK_SIZE || req->content_len > next->size)
        return reject(req, "400 Bad Request", "Image size outside slot limits");
    if (!authorized(req, expected)) return reject(req, "403 Forbidden", "Invalid signature or challenge");
    esp_fill_random(challenge, sizeof(challenge));
    uint8_t *block = malloc(BLOCK_SIZE);
    if (!block) return reject(req, "503 Service Unavailable", "Out of memory");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    esp_ota_handle_t handle = 0;
    esp_err_t err = ESP_FAIL;
    const char *reason = "Transfer interrupted or invalid image";
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 180000000LL;
    if (mbedtls_sha256_starts(&sha, 0) != 0) goto done;
    while (received < req->content_len) {
        size_t count = req->content_len - received;
        if (count > BLOCK_SIZE) count = BLOCK_SIZE;
        for (size_t used = 0; used < count;) {
            if (esp_timer_get_time() >= deadline) goto done;
            int n = httpd_req_recv(req, (char *)block + used, count - used);
            if (n <= 0) goto done;
            used += n;
        }
        if (received == 0) {
            if (!is_coordinator_image(block)) { reason = "Wrong product or chip"; goto done; }
            if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) goto done;
        }
        if (mbedtls_sha256_update(&sha, block, count) != 0 ||
            esp_ota_write(handle, block, count) != ESP_OK) goto done;
        received += count;
    }
    uint8_t actual[SHA_BYTES];
    if (mbedtls_sha256_finish(&sha, actual) != 0 || memcmp(actual, expected, sizeof(actual)) != 0) goto done;
    err = esp_ota_end(handle);
    handle = 0; // IDF releases the handle even on validation failure.
    if (err == ESP_OK) err = remember_network();
    if (err == ESP_OK) err = esp_ota_set_boot_partition(next);
done:
    if (handle) esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    free(block);
    if (err != ESP_OK) return reject(req, "400 Bad Request", reason);
    httpd_resp_sendstr(req, "Image verified. Rebooting.");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t coordinator_ota_start_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8082;
    config.ctrl_port = 8083;
    config.stack_size = 8192;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    esp_fill_random(challenge, sizeof(challenge));
    const httpd_uri_t get = { .uri = "/ota", .method = HTTP_GET, .handler = info };
    const httpd_uri_t post = { .uri = "/ota", .method = HTTP_POST, .handler = upload };
    esp_err_t err = httpd_start(&server, &config);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &get);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &post);
    if (err != ESP_OK && server) httpd_stop(server);
    if (err == ESP_OK && trial) xEventGroupSetBits(health, SERVER_READY);
    return err;
}
