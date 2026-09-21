idf_component_register(
    SRCS "monitor_app.c" "server_state.c" "monitor_config.c" "monitor_wifi.c"
         "server_monitor.c" "monitor_ui.c" "events/monitor_events.c"
         "monitor/monitor_logic.c" "network/captive_dns.c" "audio/monitor_audio.c"
         "audio/ulaw.c" "power/monitor_power.c" "ota/monitor_ota.c"
    INCLUDE_DIRS "." "events" "monitor" "network" "audio" "power" "ota"
    REQUIRES bsp esp_event esp_netif esp_timer esp_wifi nvs_flash json
             esp_http_client esp_http_server mbedtls esp_app_format esp_system esp_websocket_client spiffs app_update
)

spiffs_create_partition_image(voicefs "${CMAKE_SOURCE_DIR}/assets/music/server-monitor" FLASH_IN_PROJECT)
