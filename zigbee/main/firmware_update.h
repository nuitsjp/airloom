#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t firmware_update_register(httpd_handle_t server);
cJSON *firmware_update_status_json(void);
