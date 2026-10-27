/* Coordinator based on Espressif's CC0 Zigbee SDK thermostat and gateway examples. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "measurement.h"
#include "wifi_upload.h"

static const char *TAG = "airloom";
#define EP 1
#define BUTTON GPIO_NUM_9
#define JOIN_SECONDS 180

/* This initial gateway handles one sensor. Its address is learned on announce. */
static ezb_extaddr_t sensor_ieee;
static uint16_t sensor_short;
static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static airloom_snapshot_t latest;

static bool snapshot_copy(airloom_snapshot_t *out)
{
    portENTER_CRITICAL(&snapshot_lock);
    *out = latest;
    bool known = latest.sensor_id[0] != '\0';
    portEXIT_CRITICAL(&snapshot_lock);
    return known;
}

/* Called on the Zigbee thread, where the stack's address table is accessible. */
static bool source_id(const ezb_zcl_cmd_hdr_t *h, char id[24])
{
    ezb_extaddr_t address;
    if (h->src_addr.addr_mode == EZB_ADDR_MODE_EXT) {
        address = h->src_addr.u.extended_addr;
    } else if (h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT &&
               ezb_address_extended_by_short(h->src_addr.u.short_addr, &address) == EZB_ERR_NONE) {
        /* Resolved from Zigbee's persisted device address table. */
    } else {
        ESP_LOGW(TAG, "cannot resolve sensor IEEE address; measurement not retained");
        return false;
    }
    snprintf(id, 24, "%016" PRIx64, address.u64);
    return true;
}

enum measurement_kind { TEMPERATURE, HUMIDITY, BATTERY_PERCENT, BATTERY_VOLTAGE };
static void retain_value(const ezb_zcl_cmd_hdr_t *h, enum measurement_kind kind, float value)
{
    char id[24];
    if (!source_id(h, id)) return;
    int64_t received_us = esp_timer_get_time();
    portENTER_CRITICAL(&snapshot_lock);
    if (strcmp(latest.sensor_id, id) != 0) {
        memset(&latest, 0, sizeof latest);
        memcpy(latest.sensor_id, id, strlen(id) + 1);
    }
    switch (kind) {
    case TEMPERATURE:
        latest.has_temperature = true; latest.temperature_c = value; latest.temperature_received_us = received_us; break;
    case HUMIDITY:
        latest.has_humidity = true; latest.humidity_percent = value; latest.humidity_received_us = received_us; break;
    case BATTERY_PERCENT:
        latest.has_battery_percent = true; latest.battery_percent = value; latest.battery_percent_received_us = received_us; break;
    case BATTERY_VOLTAGE:
        latest.has_battery_voltage = true; latest.battery_voltage_v = value; latest.battery_voltage_received_us = received_us; break;
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

static void log_value(const ezb_zcl_cmd_hdr_t *h, uint16_t attr, uint8_t type, const void *value)
{
    if (!h || !value) return;
    ESP_LOGI(TAG, "attribute src_mode=%u src=0x%04x ep=%u cluster=0x%04x attr=0x%04x type=0x%02x received_uptime_ms=%" PRId64,
             h->src_addr.addr_mode, h->src_addr.u.short_addr, h->src_ep, h->cluster_id, attr, type, esp_timer_get_time()/1000);
    if (h->cluster_id == 0 && (attr == 4 || attr == 5) && type == EZB_ZCL_ATTR_TYPE_STRING) {
        const uint8_t *s = value;
        if (s[0] != 0xff) ESP_LOGI(TAG, "%s=%.*s", attr == 4 ? "manufacturer" : "model", s[0], (const char *)(s + 1));
    } else if (h->cluster_id == 0x0402 && attr == 0 && type == EZB_ZCL_ATTR_TYPE_INT16) {
        int16_t n; memcpy(&n, value, sizeof n);
        float temperature;
        if (measurement_temperature_c(n, &temperature)) {
            ESP_LOGI(TAG, "temperature=%.2f C raw=%d", temperature, n);
            retain_value(h, TEMPERATURE, temperature);
        }
        else ESP_LOGW(TAG, "temperature invalid raw=%d", n);
    } else if (h->cluster_id == 0x0405 && attr == 0 && type == EZB_ZCL_ATTR_TYPE_UINT16) {
        uint16_t n; memcpy(&n, value, sizeof n);
        float humidity;
        if (measurement_humidity_percent(n, &humidity)) {
            ESP_LOGI(TAG, "humidity=%.2f %%RH raw=%u", humidity, n);
            retain_value(h, HUMIDITY, humidity);
        }
        else ESP_LOGW(TAG, "humidity invalid raw=0x%04x", n);
    } else if (h->cluster_id == 1 && (attr == 0x20 || attr == 0x21) && type == EZB_ZCL_ATTR_TYPE_UINT8) {
        uint8_t n = *(const uint8_t *)value;
        if (n != 0xff && (attr != 0x21 || n <= 200)) {
            float battery = attr == 0x20 ? n / 10.0f : n / 2.0f;
            ESP_LOGI(TAG, "battery %s=%.1f %s raw=%u", attr == 0x20 ? "voltage" : "remaining", battery, attr == 0x20 ? "V" : "%", n);
            retain_value(h, attr == 0x20 ? BATTERY_VOLTAGE : BATTERY_PERCENT, battery);
        }
    } else {
        /* ZCL scalar types have a fixed byte length. Complex types do not
         * expose a buffer length here, so never infer their length. */
        size_t len = 0;
        if (type >= 0x08 && type <= 0x0f) len = type - 0x08 + 1;
        else if (type == 0x10 || type == 0x30) len = 1;
        else if (type >= 0x18 && type <= 0x1f) len = type - 0x18 + 1;
        else if (type >= 0x20 && type <= 0x27) len = type - 0x20 + 1;
        else if (type >= 0x28 && type <= 0x2f) len = type - 0x28 + 1;
        else if (type == 0x31) len = 2;
        ESP_LOGI(TAG, "uninterpreted attribute; type=0x%02x byte_length=%u", type, (unsigned)len);
        if (len) ESP_LOG_BUFFER_HEX(TAG, value, len);
    }
}

static void zcl_handler(ezb_zcl_core_action_callback_id_t id, void *message)
{
    if (id == EZB_ZCL_CORE_REPORT_ATTR_CB_ID) {
        ezb_zcl_cmd_report_attr_message_t *m = message;
        for (ezb_zcl_report_attr_variable_t *v = m->in.variables; v; v = v->next)
            log_value(m->in.header, v->attr_id, v->attr_type, v->attr_value);
    } else if (id == EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID) {
        ezb_zcl_cmd_read_attr_rsp_message_t *m = message;
        for (ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v; v = v->next) {
            if (v->status == EZB_ZCL_STATUS_SUCCESS) log_value(m->in.header, v->attr_id, v->attr_type, v->attr_value);
            else ESP_LOGW(TAG, "read attribute=0x%04x status=0x%02x", v->attr_id, v->status);
        }
    } else if (id == EZB_ZCL_CORE_MANUF_SPEC_CMD_CB_ID) {
        ezb_zcl_manuf_spec_cmd_message_t *m = message;
        ESP_LOGI(TAG, "manufacturer command cluster=0x%04x bytes=%u", m->info.cluster_id, m->in.payload_size);
        ESP_LOG_BUFFER_HEX(TAG, m->in.payload, m->in.payload_size);
    } else ESP_LOGI(TAG, "ZCL action=0x%04lx", (unsigned long)id);
}

static void read_basic(uint16_t addr, uint8_t ep)
{
    uint16_t attrs[] = {4, 5};
    ezb_zcl_read_attr_cmd_t req = {
        .cmd_ctrl = {.dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = addr},
                     .dst_ep = ep, .src_ep = EP, .cluster_id = 0},
        .payload = {.attr_number = 2, .attr_field = attrs},
    };
    ESP_LOGI(TAG, "basic read request status=%d", ezb_zcl_read_attr_cmd_req(&req));
}

static void bind_result(const ezb_zdp_bind_req_result_t *result, void *ctx)
{
    uint16_t cluster = (uint16_t)(uintptr_t)ctx;
    if (result->error || !result->rsp) {
        ESP_LOGW(TAG, "bind cluster=0x%04x error=%d", cluster, result->error);
    } else {
        ESP_LOGI(TAG, "bind cluster=0x%04x status=0x%02x", cluster, result->rsp->status);
    }
}

static void setup_measurement(uint16_t addr, uint8_t ep, uint16_t cluster)
{
    uint16_t attrs[] = {0, 0};
    uint8_t count = 1;
    if (cluster == 0x0001) {
        attrs[0] = 0x0020;
        attrs[1] = 0x0021;
        count = 2;
    }
    ezb_zcl_read_attr_cmd_t read = {
        .cmd_ctrl = {.dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = addr},
                     .dst_ep = ep, .src_ep = EP, .cluster_id = cluster},
        .payload = {.attr_number = count, .attr_field = attrs},
    };
    ESP_LOGI(TAG, "read cluster=0x%04x request status=%d", cluster, ezb_zcl_read_attr_cmd_req(&read));

    if (addr != sensor_short) return;
    ezb_extaddr_t local;
    ezb_get_extended_address(&local);
    ezb_zdo_bind_req_t bind = {
        .dst_nwk_addr = addr,
        .field = {.src_addr = sensor_ieee, .src_ep = ep, .cluster_id = cluster,
                  .dst_addr_mode = EZB_ADDR_MODE_EXT, .dst_addr.extended_addr = local, .dst_ep = EP},
        .cb = bind_result,
        .user_ctx = (void *)(uintptr_t)cluster,
    };
    ESP_LOGI(TAG, "bind cluster=0x%04x request status=%d", cluster, ezb_zdo_bind_req(&bind));
}

static void simple_desc(const ezb_zdo_simple_desc_req_result_t *result, void *ctx)
{
    (void)ctx;
    if (result->error || !result->rsp || result->rsp->status) {
        ESP_LOGW(TAG, "simple descriptor failed error=%d", result->error); return;
    }
    ezb_zdp_simple_desc_rsp_field_t *r = result->rsp;
    ezb_af_simple_desc_t *d = &r->desc;
    ESP_LOGI(TAG, "device=0x%04x ep=%u profile=0x%04x device_id=0x%04x", r->nwk_addr_of_interest, d->ep_id, d->app_profile_id, d->app_device_id);
    for (int i = 0; i < d->app_input_cluster_count + d->app_output_cluster_count; ++i) {
        ESP_LOGI(TAG, "ep=%u %s cluster=0x%04x", d->ep_id, i < d->app_input_cluster_count ? "server" : "client", d->app_cluster_list[i]);
        if (i < d->app_input_cluster_count && d->app_cluster_list[i] == 0) read_basic(r->nwk_addr_of_interest, d->ep_id);
        if (i < d->app_input_cluster_count && (d->app_cluster_list[i] == 0x0001 ||
            d->app_cluster_list[i] == 0x0402 || d->app_cluster_list[i] == 0x0405))
            setup_measurement(r->nwk_addr_of_interest, d->ep_id, d->app_cluster_list[i]);
    }
}

static void active_eps(const ezb_zdo_active_ep_req_result_t *result, void *ctx)
{
    (void)ctx;
    if (result->error || !result->rsp || result->rsp->status) {
        ESP_LOGW(TAG, "active endpoints failed error=%d", result->error); return;
    }
    for (int i = 0; i < result->rsp->active_ep_count; ++i) {
        uint16_t addr = result->rsp->nwk_addr_of_interest;
        ezb_zdo_simple_desc_req_t req = {.dst_nwk_addr = addr, .field = {.nwk_addr_of_interest = addr, .endpoint = result->rsp->active_ep_list[i]}, .cb = simple_desc};
        ESP_LOGI(TAG, "simple descriptor request status=%d", ezb_zdo_simple_desc_req(&req));
    }
}

static bool signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    if (type == EZB_ZDO_SIGNAL_SKIP_STARTUP) {
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
    } else if (type == EZB_BDB_SIGNAL_DEVICE_FIRST_START || type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "startup status=%d factory_new=%d", status, ezb_bdb_is_factory_new());
        if (status == EZB_BDB_STATUS_SUCCESS && ezb_bdb_is_factory_new()) ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
    } else if (type == EZB_BDB_SIGNAL_FORMATION) {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "formation status=%d pan=0x%04x channel=%d", status, ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
    } else if (type == EZB_ZDO_SIGNAL_DEVICE_ANNCE) {
        const ezb_zdo_signal_device_annce_params_t *p = ezb_app_signal_get_params(signal);
        sensor_short = p->short_addr;
        sensor_ieee = p->device_addr;
        ESP_LOGI(TAG, "device announced short=0x%04x ieee=%016llx capability=0x%02x", p->short_addr, p->device_addr.u64, p->capability);
        ezb_zdo_active_ep_req_t req = {.dst_nwk_addr = p->short_addr, .field.nwk_addr_of_interest = p->short_addr, .cb = active_eps};
        ESP_LOGI(TAG, "active endpoints request status=%d", ezb_zdo_active_ep_req(&req));
    } else if (type == EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS) {
        ESP_LOGI(TAG, "permit join remaining=%u seconds", *(uint8_t *)ezb_app_signal_get_params(signal));
    } else ESP_LOGI(TAG, "signal=%s", ezb_app_signal_to_string(type));
    return true;
}

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {.pin_bit_mask = 1ULL << BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&io));
    bool was_down = false;
    for (;;) {
        bool down = gpio_get_level(BUTTON) == 0;
        if (down && !was_down) {
            esp_zigbee_lock_acquire(portMAX_DELAY);
            int err = ezb_bdb_open_network(JOIN_SECONDS);
            esp_zigbee_lock_release();
            ESP_LOGI(TAG, "button: permit join %d seconds status=%d", JOIN_SECONDS, err);
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t cfg = {
        .device_config = {.device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, .install_code_policy = false, .zczr_config.max_children = 10},
        .platform_config = {.storage_partition_name = "nvs", .radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    if (!wifi_upload_start(snapshot_copy)) ESP_LOGE(TAG, "Wi-Fi upload task could not be started");
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(0x07fff800));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(signal_handler));
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_custom_gateway_config_t gc = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_custom_gateway(EP, &gc);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));
    ezb_zcl_core_action_handler_register(zcl_handler);
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    xTaskCreate(button_task, "button", 2048, NULL, 4, NULL);
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
}
