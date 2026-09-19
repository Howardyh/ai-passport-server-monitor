#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t version;
    char ssid[33], password[65], url[256], token[129];
} monitor_config_t;
bool monitor_config_valid(const monitor_config_t *config);
esp_err_t monitor_config_load(monitor_config_t *config);
esp_err_t monitor_config_save(const monitor_config_t *config);
