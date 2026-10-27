idf_component_register(SRCS "main.c" "network.c" "storage.c" "web_server.c" "firmware_update.c" "../../measurement/measurement.c"
    INCLUDE_DIRS "." "../../measurement"
    REQUIRES esp_coex nvs_flash esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_server esp_app_format app_update json littlefs
    EMBED_TXTFILES "dashboard.html")
