#include "network.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

#define CONNECTED BIT0
#define FAILED BIT1
static const char *TAG = "airloom_network";
static EventGroupHandle_t events;
static TaskHandle_t reconnect_task;

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (esp_wifi_connect() != ESP_OK) xEventGroupSetBits(events, FAILED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u", event->reason);
        xEventGroupClearBits(events, CONNECTED);
        xEventGroupSetBits(events, FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Web address http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(events, FAILED);
        xEventGroupSetBits(events, CONNECTED);
    }
    if (reconnect_task) xTaskNotifyGive(reconnect_task);
}

static void reconnect(void *arg)
{
    (void)arg;
    for (;;) {
        if (!(xEventGroupGetBits(events) & CONNECTED)) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi reconnect: %s", esp_err_to_name(err));
            /* Notifications cannot shorten the retry interval after a failed attempt. */
            vTaskDelay(pdMS_TO_TICKS(30000));
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

bool network_start(void)
{
    if (!CONFIG_AIRLOOM_WIFI_SSID[0]) {
        ESP_LOGE(TAG, "Wi-Fi SSID is not configured");
        return false;
    }
    events = xEventGroupCreate();
    if (!events) return false;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) return false;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t config = {0};
    size_t ssid_size = strlen(CONFIG_AIRLOOM_WIFI_SSID);
    size_t password_size = strlen(CONFIG_AIRLOOM_WIFI_PASSWORD);
    if (ssid_size > sizeof(config.sta.ssid) || password_size > sizeof(config.sta.password)) return false;
    memcpy(config.sta.ssid, CONFIG_AIRLOOM_WIFI_SSID, ssid_size);
    memcpy(config.sta.password, CONFIG_AIRLOOM_WIFI_PASSWORD, password_size);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    memset(&config, 0, sizeof(config));
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(events, CONNECTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & CONNECTED)) ESP_LOGW(TAG, "Initial Wi-Fi connection failed or timed out; Zigbee will continue");
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&time_config));
    return xTaskCreate(reconnect, "network", 3072, NULL, 3, &reconnect_task) == pdPASS;
}
