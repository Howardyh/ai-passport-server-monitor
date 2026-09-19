#pragma once
#include "bsp_button.h"
#include "monitor_wifi.h"
#include "server_state.h"
/* Caller holds BSP LVGL lock. Slow work is dispatched only. */
void monitor_ui_init(void);
void monitor_ui_key(bsp_btn_t button, bsp_btn_ev_t event);
void monitor_ui_update(const server_state_t *state, const monitor_wifi_view_t *wifi, int battery, uint64_t now_ms);
