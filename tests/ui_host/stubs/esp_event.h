#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *,esp_event_base_t,int32_t,void *);
#define ESP_EVENT_DECLARE_BASE(x) extern esp_event_base_t x
