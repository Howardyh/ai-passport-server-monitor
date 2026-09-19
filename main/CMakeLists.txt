idf_component_register(
    SRCS "monitor_app.c" "server_state.c" "monitor_config.c" "monitor_wifi.c"
         "server_monitor.c" "monitor_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_netif esp_timer esp_wifi nvs_flash json
             esp_http_client esp_http_server mbedtls esp_app_format esp_system
)
