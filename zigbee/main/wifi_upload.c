#include "wifi_upload.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define CONNECTED_BIT BIT0
#define FAILED_BIT BIT1
#define CONNECT_TIMEOUT_MS 20000
#define HTTP_TIMEOUT_MS 10000
#define WIFI_RETRY_INTERVAL_US (30LL * 1000000)
#define HTTP_RETRY_INTERVAL_US (10LL * 1000000)
#define SENSOR_POLL_INTERVAL_US (5LL * 1000000)

static const char *TAG = "airloom_upload";
static EventGroupHandle_t wifi_events;
static airloom_snapshot_fn snapshot_fn;
static TaskHandle_t upload_task_handle;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (esp_wifi_connect() != ESP_OK) xEventGroupSetBits(wifi_events, FAILED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u", event->reason);
        xEventGroupClearBits(wifi_events, CONNECTED_BIT);
        xEventGroupSetBits(wifi_events, FAILED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected; IP address acquired");
        xEventGroupClearBits(wifi_events, FAILED_BIT);
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
    }
    if (upload_task_handle) xTaskNotifyGive(upload_task_handle);
}

static bool wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) goto failed;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) goto failed;
    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "failed to create Wi-Fi interface");
        return false;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto failed;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) goto failed;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto failed;
    wifi_config_t config = {0};
    size_t ssid_len = strlen(CONFIG_AIRLOOM_WIFI_SSID);
    size_t password_len = strlen(CONFIG_AIRLOOM_WIFI_PASSWORD);
    if (ssid_len > sizeof(config.sta.ssid) || password_len > sizeof(config.sta.password)) {
        ESP_LOGE(TAG, "Wi-Fi credentials exceed the supported length");
        return false;
    }
    memcpy(config.sta.ssid, CONFIG_AIRLOOM_WIFI_SSID, ssid_len);
    memcpy(config.sta.password, CONFIG_AIRLOOM_WIFI_PASSWORD, password_len);
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) goto failed;
    return true;
failed:
    ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(err));
    return false;
}

static void add_value(cJSON *root, const char *name, const char *age_name, const char *stale_name,
                      bool present, float value, int64_t received_us, int64_t now_us)
{
    if (!present) {
        cJSON_AddNullToObject(root, name);
        cJSON_AddNullToObject(root, age_name);
        if (stale_name) cJSON_AddNullToObject(root, stale_name);
        return;
    }
    double age = (double)(now_us - received_us) / 1000000.0;
    if (age < 0) age = 0;
    cJSON_AddNumberToObject(root, name, value);
    cJSON_AddNumberToObject(root, age_name, age);
    if (stale_name) cJSON_AddBoolToObject(root, stale_name, age >= CONFIG_AIRLOOM_STALE_SECONDS);
}

static bool send_snapshot(void)
{
    airloom_snapshot_t sample = {0};
    if (!snapshot_fn(&sample)) {
        ESP_LOGI(TAG, "no sensor data available; upload skipped");
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "JSON allocation failed");
        return false;
    }
    cJSON_AddStringToObject(root, "sensor_id", sample.sensor_id);
    cJSON_AddNumberToObject(root, "gateway_uptime_seconds", now_us / 1000000.0);
    add_value(root, "temperature_c", "temperature_age_seconds", "temperature_stale",
              sample.has_temperature, sample.temperature_c, sample.temperature_received_us, now_us);
    add_value(root, "humidity_percent", "humidity_age_seconds", "humidity_stale",
              sample.has_humidity, sample.humidity_percent, sample.humidity_received_us, now_us);
    if (sample.has_battery_percent) {
        add_value(root, "battery_percent", "battery_percent_age_seconds", NULL, true,
                  sample.battery_percent, sample.battery_percent_received_us, now_us);
    }
    if (sample.has_battery_voltage) {
        add_value(root, "battery_voltage_v", "battery_voltage_age_seconds", NULL, true,
                  sample.battery_voltage_v, sample.battery_voltage_received_us, now_us);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        ESP_LOGE(TAG, "JSON serialization failed");
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = CONFIG_AIRLOOM_HTTP_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client initialization failed");
        free(body);
        return false;
    }
    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, strlen(body));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP response status=%d", status);
        if (status < 200 || status >= 300) ESP_LOGW(TAG, "upload failed");
        else success = true;
    } else {
        ESP_LOGW(TAG, "HTTP upload failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    free(body);
    return success;
}

static void upload_task(void *arg)
{
    (void)arg;
    int64_t next_upload_us = 0;
    int64_t next_retry_us = 0;
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        int64_t wake_us;
        if (xEventGroupGetBits(wifi_events) & CONNECTED_BIT) {
            next_retry_us = 0;
            if (now_us >= next_upload_us) {
                airloom_snapshot_t probe = {0};
                if (snapshot_fn(&probe) && (probe.has_temperature || probe.has_humidity)) {
                    bool success = send_snapshot();
                    next_upload_us = esp_timer_get_time() + (success
                        ? (int64_t)CONFIG_AIRLOOM_UPLOAD_INTERVAL_SECONDS * 1000000
                        : HTTP_RETRY_INTERVAL_US);
                } else {
                    next_upload_us = now_us + SENSOR_POLL_INTERVAL_US;
                }
            }
            wake_us = next_upload_us;
        } else {
            if (now_us >= next_retry_us) {
                xEventGroupClearBits(wifi_events, FAILED_BIT);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
                next_retry_us = now_us + WIFI_RETRY_INTERVAL_US;
            }
            wake_us = next_retry_us;
        }
        int64_t wait_us = wake_us - esp_timer_get_time();
        uint32_t wait_ms = wait_us <= 0 ? 1 : wait_us > 60000000 ? 60000 : (uint32_t)((wait_us + 999) / 1000);
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        ulTaskNotifyTake(pdTRUE, wait_ticks ? wait_ticks : 1);
    }
}

bool wifi_upload_start(airloom_snapshot_fn snapshot)
{
    if (!snapshot) return false;
    if (!CONFIG_AIRLOOM_WIFI_SSID[0] || !CONFIG_AIRLOOM_HTTP_URL[0]) {
        ESP_LOGW(TAG, "Wi-Fi SSID or HTTP URL not configured; upload disabled");
        return false;
    }
    snapshot_fn = snapshot;
    wifi_events = xEventGroupCreate();
    if (!wifi_events) {
        ESP_LOGE(TAG, "could not allocate Wi-Fi event group");
        return false;
    }
    /* The Wi-Fi driver normally logs the configured SSID. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    if (!wifi_init()) return false;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_events, CONNECTED_BIT | FAILED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (!(bits & CONNECTED_BIT)) ESP_LOGW(TAG, "initial Wi-Fi connection failed or timed out");
    return xTaskCreate(upload_task, "upload", 8192, NULL, 3, &upload_task_handle) == pdPASS;
}
