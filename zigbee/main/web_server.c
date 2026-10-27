#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "storage.h"
#include "firmware_update.h"

static const char *TAG = "airloom_web";
extern const unsigned char dashboard_start[] asm("_binary_dashboard_html_start");
extern const unsigned char dashboard_end[] asm("_binary_dashboard_html_end");

static esp_err_t unavailable(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *value)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!value) {
        return unavailable(req, "Data unavailable");
    }
    char *body = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (!body) {
        return unavailable(req, "Insufficient memory");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t result = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    return result;
}

static esp_err_t dashboard(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)dashboard_start,
                           dashboard_end - dashboard_start - 1);
}

static esp_err_t status(httpd_req_t *req)
{
    return send_json(req, storage_status_json());
}

static bool query_number(const char *query, const char *name, unsigned minimum,
                         unsigned maximum, unsigned *out)
{
    char text[12];
    esp_err_t result = httpd_query_key_value(query, name, text, sizeof(text));
    if (result == ESP_ERR_NOT_FOUND) return true;
    if (result != ESP_OK || !text[0]) return false;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    unsigned long value = strtoul(text, NULL, 10);
    if (value < minimum || value > maximum) return false;
    *out = (unsigned)value;
    return true;
}

static esp_err_t history(httpd_req_t *req)
{
    unsigned limit = 32, offset = 0;
    size_t length = httpd_req_get_url_query_len(req);
    if (length) {
        char query[64];
        if (length >= sizeof(query) ||
            httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            !query_number(query, "limit", 1, 64, &limit) ||
            !query_number(query, "offset", 0, 4096, &offset)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "limit must be 1..64 and offset must be 0..4096");
        }
    }
    return send_json(req, storage_history_json(limit, offset));
}

bool web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    httpd_handle_t server = NULL;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(result));
        return false;
    }
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = dashboard},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/history", .method = HTTP_GET, .handler = history},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        result = httpd_register_uri_handler(server, &handlers[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "HTTP handler registration failed: %s", esp_err_to_name(result));
            httpd_stop(server);
            return false;
        }
    }
    if (firmware_update_register(server) != ESP_OK) {
        httpd_stop(server);
        return false;
    }
    ESP_LOGI(TAG, "HTTP server listening on port %u", config.server_port);
    return true;
}
