#pragma once
#include "monitor_config.h"

typedef struct {
    bool connected, provisioning;
    int rssi;
    uint32_t revision;
    char ssid[33], api_host[96], message[40];
    char setup_ssid[33], setup_key[17]; /* ephemeral AP key, never saved Wi-Fi password */
    unsigned setup_seconds;
} monitor_wifi_view_t;
esp_err_t monitor_wifi_start(void);
void monitor_wifi_view(monitor_wifi_view_t *view);
bool monitor_wifi_config(monitor_config_t *config, uint32_t *revision);
void monitor_wifi_reconnect(void);
void monitor_wifi_provision(void);
