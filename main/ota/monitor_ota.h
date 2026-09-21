#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef struct {bool available,busy;char version[32],message[64];} monitor_ota_view_t;
esp_err_t monitor_ota_start(void);
void monitor_ota_view(monitor_ota_view_t *view);
void monitor_ota_self_check(bool healthy);
