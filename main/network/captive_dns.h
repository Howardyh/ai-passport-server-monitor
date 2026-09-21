#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t captive_dns_start(void);
void captive_dns_enable(bool enabled);
