#pragma once
#include <stdbool.h>
#include <stdint.h>
bool monitor_power_key(uint64_t now);
void monitor_power_tick(uint64_t now);
bool monitor_power_ambient(void);
