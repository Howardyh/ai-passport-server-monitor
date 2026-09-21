#pragma once
#include "esp_err.h"
#include "server_state.h"
esp_err_t server_monitor_start(void);
void server_monitor_refresh(void);
void server_monitor_snapshot(server_state_t *state);

void server_monitor_pause(bool pause);
bool server_monitor_progress(void);
bool server_monitor_quiescent(void);
