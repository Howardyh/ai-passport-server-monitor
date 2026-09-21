#pragma once
#include "esp_event.h"
#include <stdint.h>
ESP_EVENT_DECLARE_BASE(MONITOR_EVENTS);
typedef enum {
    EV_SYSTEM_BOOTED, EV_PROVISIONING_STARTED,
    EV_WIFI_CONNECTING, EV_WIFI_CONNECTED, EV_WIFI_FAILED, EV_WIFI_DISCONNECTED,
    EV_WSS_CONNECTING, EV_WSS_CONNECTED, EV_WSS_DISCONNECTED, EV_HTTPS_FALLBACK_STARTED,
    EV_SERVER_SYNC_FIRST_SUCCESS, EV_SERVER_SYNC_FAILED, EV_SERVER_ONLINE,
    EV_SERVER_OFFLINE, EV_SERVER_RECOVERED, EV_SERVER_ALERT,
    EV_OTA_AVAILABLE, EV_OTA_STARTED, EV_OTA_SUCCESS, EV_OTA_FAILED, EV_LOW_BATTERY,
    EV_STATE_CHANGED, EV_CONFIG_CHANGED, EV_COMMAND, EV_COUNT
} monitor_event_id_t;
typedef enum { CMD_SETUP, CMD_REFRESH, CMD_MUTE, CMD_SETTING, CMD_OTA_CHECK, CMD_OTA_INSTALL } monitor_command_t;
typedef struct { int command, setting, value; } monitor_command_data_t;
typedef struct { int level; float value; char source[24], message[96]; uint64_t seq, timestamp; } monitor_alert_t;
esp_err_t monitor_events_init(void);
esp_err_t monitor_subscribe(int32_t id, esp_event_handler_t handler, void *arg);
void monitor_emit(monitor_event_id_t id, const void *data, size_t size);
void monitor_command(monitor_command_t cmd, int setting, int value);
uint32_t monitor_events_dropped(void);
