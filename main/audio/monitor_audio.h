#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t monitor_audio_start(void);
unsigned monitor_audio_depth(void);
unsigned monitor_audio_errors(void);
int16_t monitor_ulaw_decode(uint8_t value);
