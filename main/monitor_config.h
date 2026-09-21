#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { SET_VOICE, SET_VOLUME, SET_STARTUP, SET_NETWORK, SET_SERVER, SET_CRITICAL, SET_ALERT_SOUND,
    SET_BYPASS, SET_BRIGHTNESS, SET_TIMEOUT, SET_POWER, SET_COUNT } monitor_setting_t;
typedef struct {
    uint32_t version;
    char ssid[33], password[65], url[256], token[129], ws_url[256];
    char server_name[64], host[96], device_name[33], timezone[64];
    uint8_t audio_enabled, volume, startup_voice, network_voice, server_voice, critical_alerts;
    uint8_t alert_sound, critical_bypass_mute, brightness, power_mode;
    uint32_t screen_timeout;
} monitor_config_t;
void monitor_config_defaults(monitor_config_t *config);
bool monitor_config_valid(const monitor_config_t *config);
esp_err_t monitor_config_init(void);
esp_err_t monitor_config_load(monitor_config_t *config);
esp_err_t monitor_config_save(const monitor_config_t *config);
void monitor_config_snapshot(monitor_config_t *config);
esp_err_t monitor_config_setting(monitor_setting_t setting, int value);
