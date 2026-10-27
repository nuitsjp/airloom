#include "firmware_update.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PREFIX_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define UPDATE_CHUNK_SIZE 4096
#define MAX_REJECT_DRAIN_SIZE 0x170000U
#define PASSWORD_HEADER "X-Airloom-Update-Password"
static const char *TAG = "airloom_update";
static atomic_bool updating;

/* Closing a socket with unread TCP data can reset it before the client receives
 * the error response. Drain only a bounded, known body; each receive retains the
 * server's configured timeout, and a closed/timed-out connection stops draining. */
static esp_err_t reject(httpd_req_t *req, const char *status, const char *message,
                        size_t unread_remaining)
{
    if (req->content_len <= MAX_REJECT_DRAIN_SIZE && unread_remaining <= req->content_len) {
        char discarded[512];
        while (unread_remaining) {
            size_t wanted = unread_remaining < sizeof discarded ? unread_remaining : sizeof discarded;
            int length = httpd_req_recv(req, discarded, wanted);
            if (length <= 0) break;
            unread_remaining -= (size_t)length;
        }
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static bool password_configured(void)
{
    size_t length = strlen(CONFIG_AIRLOOM_UPDATE_PASSWORD);
    return length >= 12 && length <= 64;
}

static bool authorized(httpd_req_t *req)
{
    size_t expected_length = strlen(CONFIG_AIRLOOM_UPDATE_PASSWORD);
    size_t length = httpd_req_get_hdr_value_len(req, PASSWORD_HEADER);
    if (length != expected_length || !length) return false;
    char *password = malloc(length + 1);
    if (!password) return false;
    bool read = httpd_req_get_hdr_value_str(req, PASSWORD_HEADER, password, length + 1) == ESP_OK;
    if (!read) { free(password); return false; }
    unsigned char different = 0;
    for (size_t i = 0; i < length; ++i)
        different |= (unsigned char)password[i] ^ (unsigned char)CONFIG_AIRLOOM_UPDATE_PASSWORD[i];
    memset(password, 0, length + 1);
    free(password);
    return different == 0;
}

static bool same_origin(httpd_req_t *req)
{
    size_t length = httpd_req_get_hdr_value_len(req, "Origin");
    if (!length) return true; /* Non-browser clients need not send Origin. */
    char origin[128], host[96];
    if (length >= sizeof origin ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK)
        return false;
    return !strncmp(origin, "http://", 7) && !strcmp(origin + 7, host);
}

static bool valid_prefix(const unsigned char *prefix, esp_app_desc_t *description)
{
    esp_image_header_t header;
    esp_image_segment_header_t segment;
    memcpy(&header, prefix, sizeof header);
    memcpy(&segment, prefix + sizeof header, sizeof segment);
    memcpy(description, prefix + sizeof header + sizeof segment, sizeof *description);
    return header.magic == ESP_IMAGE_HEADER_MAGIC && header.chip_id == ESP_CHIP_ID_ESP32C6 &&
        header.segment_count > 0 && header.segment_count <= ESP_IMAGE_MAX_SEGMENTS &&
        segment.data_len >= sizeof *description &&
        description->magic_word == ESP_APP_DESC_MAGIC_WORD &&
        memchr(description->project_name, '\0', sizeof description->project_name) &&
        !strcmp(description->project_name, "airloom_zigbee");
}

static void hex_hash(const uint8_t bytes[32], char text[65])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        text[2 * i] = hex[bytes[i] >> 4];
        text[2 * i + 1] = hex[bytes[i] & 15];
    }
    text[64] = '\0';
}

cJSON *firmware_update_status_json(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    char hash[65];
    hex_hash(esp_app_get_description()->app_elf_sha256, hash);
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    if (!cJSON_AddStringToObject(json, "version", esp_app_get_description()->version) ||
        !cJSON_AddStringToObject(json, "elf_sha256", hash) ||
        !cJSON_AddStringToObject(json, "running_partition", running ? running->label : "") ||
        !cJSON_AddNumberToObject(json, "update_capacity_bytes", next ? next->size : 0) ||
        !cJSON_AddBoolToObject(json, "updating", atomic_load(&updating)) ||
        !cJSON_AddBoolToObject(json, "password_configured", password_configured())) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *json = firmware_update_status_json();
    if (!json) return reject(req, "503 Service Unavailable", "Insufficient memory", req->content_len);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return reject(req, "503 Service Unavailable", "Insufficient memory", req->content_len);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t error = httpd_resp_send(req, text, strlen(text));
    cJSON_free(text);
    return error;
}

static void restart_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!same_origin(req)) return reject(req, "403 Forbidden", "Cross-origin update is not allowed", req->content_len);
    if (!password_configured()) return reject(req, "503 Service Unavailable", "Firmware update password is not configured", req->content_len);
    if (!authorized(req)) return reject(req, "401 Unauthorized", "Firmware update password is required", req->content_len);
    if (atomic_load(&updating)) return reject(req, "409 Conflict", "A firmware update is already in progress", req->content_len);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!running || !target || target->address == running->address)
        return reject(req, "503 Service Unavailable", "An inactive OTA partition is unavailable", req->content_len);
    if (req->content_len < PREFIX_SIZE)
        return reject(req, "400 Bad Request", "The firmware image is too short", req->content_len);
    if (req->content_len > target->size)
        return reject(req, "413 Content Too Large", "The firmware image exceeds the update partition", 0);
    char content_type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof content_type) != ESP_OK ||
        strcmp(content_type, "application/octet-stream"))
        return reject(req, "415 Unsupported Media Type", "Send the raw application .bin as application/octet-stream", req->content_len);
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding"))
        return reject(req, "400 Bad Request", "Chunked firmware uploads are not supported", 0);

    unsigned char prefix[PREFIX_SIZE];
    size_t received = 0;
    while (received < sizeof prefix) {
        int length = httpd_req_recv(req, (char *)prefix + received, sizeof prefix - received);
        if (length <= 0) return reject(req, "400 Bad Request", "Firmware upload was interrupted", 0);
        received += (size_t)length;
    }
    esp_app_desc_t description;
    if (!valid_prefix(prefix, &description))
        return reject(req, "400 Bad Request", "Expected an ESP32-C6 airloom_zigbee application image", req->content_len - received);
    unsigned char *buffer = malloc(UPDATE_CHUNK_SIZE);
    if (!buffer) return reject(req, "503 Service Unavailable", "Insufficient memory", req->content_len - received);
    atomic_store(&updating, true);
    esp_ota_handle_t handle;
    esp_err_t error = esp_ota_begin(target, req->content_len, &handle);
    bool handle_open = error == ESP_OK;
    bool transfer_interrupted = false;
    if (error == ESP_OK) error = esp_ota_write(handle, prefix, sizeof prefix);
    while (error == ESP_OK && received < req->content_len) {
        size_t remaining = req->content_len - received;
        size_t wanted = remaining < UPDATE_CHUNK_SIZE ? remaining : UPDATE_CHUNK_SIZE;
        int length = httpd_req_recv(req, (char *)buffer, wanted);
        if (length <= 0) { error = ESP_ERR_INVALID_SIZE; transfer_interrupted = true; break; }
        error = esp_ota_write(handle, buffer, (size_t)length);
        received += (size_t)length;
    }
    free(buffer);
    if (error == ESP_OK) {
        error = esp_ota_end(handle);
        handle_open = false; /* esp_ota_end frees the handle even on failure. */
    }
    if (error != ESP_OK) {
        if (handle_open) esp_ota_abort(handle);
        atomic_store(&updating, false);
        ESP_LOGW(TAG, "Update rejected or interrupted: %s", esp_err_to_name(error));
        return reject(req, error == ESP_ERR_OTA_VALIDATE_FAILED || error == ESP_ERR_INVALID_SIZE ?
            "400 Bad Request" : "500 Internal Server Error", "Firmware validation or transfer failed; current firmware retained",
            transfer_interrupted ? 0 : req->content_len - received);
    }
    TaskHandle_t restart_handle = NULL;
    if (xTaskCreate(restart_task, "ota_restart", 2048, NULL, 3, &restart_handle) != pdPASS) {
        atomic_store(&updating, false);
        return reject(req, "503 Service Unavailable", "Cannot schedule restart; current firmware retained", 0);
    }
    error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) {
        vTaskDelete(restart_handle);
        atomic_store(&updating, false);
        ESP_LOGE(TAG, "Cannot select OTA partition: %s", esp_err_to_name(error));
        return reject(req, "500 Internal Server Error", "Cannot select update; current firmware retained", 0);
    }
    char hash[65];
    hex_hash(description.app_elf_sha256, hash);
    char response[128];
    snprintf(response, sizeof response, "{\"ok\":true,\"rebooting\":true,\"elf_sha256\":\"%s\"}", hash);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "Firmware verified; selected partition %s for next boot", target->label);
    esp_err_t sent = httpd_resp_send(req, response, strlen(response));
    xTaskNotifyGive(restart_handle);
    return sent;
}

esp_err_t firmware_update_register(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/api/firmware", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/firmware", .method = HTTP_POST, .handler = upload_handler},
    };
    for (unsigned i = 0; i < sizeof handlers / sizeof handlers[0]; ++i) {
        esp_err_t error = httpd_register_uri_handler(server, &handlers[i]);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}
