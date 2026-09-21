#include "bsp_battery.h"
#include "bsp_audio.h"
#include "monitor_events.h"
#include "monitor_config.h"
#include "monitor_audio.h"
#include "monitor_power.h"
#include "monitor_ota.h"
#include <stdatomic.h>
#include "bsp_button.h"
#include "bsp_display.h"
#include "monitor_ui.h"
#include "monitor_wifi.h"
#include "server_monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "server_monitor";
typedef struct { bsp_btn_t button; bsp_btn_ev_t event; } input_t;
static QueueHandle_t s_input;
static QueueHandle_t s_battery;
static QueueHandle_t s_control;
static bool s_core_ok;
static atomic_bool s_dirty;
static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *arg) {
    (void)arg;
    input_t input = {.button = button, .event = event};
    if (s_input) (void)xQueueSend(s_input, &input, 0);
}
static void telemetry_task(void *arg) {
    (void)arg;
    bool battery_ok = bsp_battery_init() == ESP_OK;
    uint64_t next_log = 0;
    bool low=false;
    for (;;) {
        int battery = battery_ok ? bsp_battery_soc() : -1;
        xQueueOverwrite(s_battery, &battery);
        if(battery>=0 && battery<=15 && !low) monitor_emit(EV_LOW_BATTERY,NULL,0);
        if(battery>=0) low=battery<=15;
        uint64_t now = (uint64_t)esp_timer_get_time()/1000;
        if (now >= next_log) {
            ESP_LOGI(TAG, "heap_free=%u minimum_free_heap=%u largest_free_block=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            next_log = now + 30000;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
static void ui_task(void *arg) {
    (void)arg;
    int battery = -1;
    uint64_t next_ui = 0;
    unsigned rendered=0;
    bool wake_guard=false;
    for (;;) {
        input_t input;
        bool key = xQueueReceive(s_input, &input, pdMS_TO_TICKS(100)) == pdTRUE;
        uint64_t now = (uint64_t)esp_timer_get_time()/1000;
        if (s_battery) (void)xQueueReceive(s_battery, &battery, 0);
        if(key) {
            if(monitor_power_key(now)) wake_guard=true;
            if(wake_guard) {key=false;if(input.event==BSP_BTN_CLICK||input.event==BSP_BTN_LONG||input.event==BSP_BTN_DOUBLE) wake_guard=false;}
        }
        monitor_power_tick(now);
        if (key || now >= next_ui || atomic_exchange(&s_dirty,false)) {
            server_state_t state; monitor_wifi_view_t wifi;
            server_monitor_snapshot(&state); monitor_wifi_view(&wifi);
            if (bsp_lvgl_lock(50)) {
                if (key) monitor_ui_key(input.button, input.event);
                monitor_ui_update(&state, &wifi, battery, now);
                rendered++;
                bsp_lvgl_unlock();
            }
            next_ui = now + 1000;
            if(now>30000) monitor_ota_self_check(s_core_ok&&rendered>=20&&server_monitor_progress());
        }
    }
}
static void app_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;atomic_store(&s_dirty,true);
    if(id==EV_COMMAND) {monitor_command_data_t *cmd=data;if(cmd->command==CMD_SETTING||cmd->command==CMD_MUTE) (void)xQueueSend(s_control,cmd,0);}
}
static void control_task(void *arg) {
    (void)arg;monitor_command_data_t cmd;
    for(;;) if(xQueueReceive(s_control,&cmd,portMAX_DELAY)==pdTRUE) {
        if(cmd.command==CMD_MUTE) {monitor_config_t c;monitor_config_snapshot(&c);monitor_config_setting(SET_VOICE,!c.audio_enabled);}
        else monitor_config_setting((monitor_setting_t)cmd.setting,cmd.value);
    }
}
void app_main(void) {
    ESP_LOGI(TAG, "AI Passport Server Monitor starting");
    if(monitor_events_init()!=ESP_OK) return;
    bool nvs_ok=monitor_config_init()==ESP_OK;
    if(!nvs_ok) ESP_LOGE(TAG,"NVS unavailable; preserved without erase");
    bool audio_ok=bsp_audio_init()==ESP_OK;
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) { ESP_LOGE(TAG, "Display unavailable"); return; }
    if (!bsp_lvgl_lock(1000)) return;
    monitor_ui_init(); bsp_lvgl_unlock(); bsp_display_backlight(80);
    s_input = xQueueCreate(12, sizeof(input_t));
    if (!s_input) { ESP_LOGE(TAG, "Input queue unavailable"); return; }
    if (bsp_button_init(on_button, NULL) != ESP_OK) ESP_LOGE(TAG, "Buttons unavailable");
    s_control=xQueueCreate(8,sizeof(monitor_command_data_t));
    if(!s_control||xTaskCreate(control_task,"config_worker",4096,NULL,2,NULL)!=pdPASS) return;
    if(monitor_subscribe(ESP_EVENT_ANY_ID,app_event,NULL)!=ESP_OK) return;
    if(audio_ok&&monitor_audio_start()!=ESP_OK) ESP_LOGE(TAG,"VoiceFS/audio unavailable");
    monitor_emit(EV_SYSTEM_BOOTED,NULL,0);
    bool wifi_ok=monitor_wifi_start()==ESP_OK;
    bool network_ok=server_monitor_start()==ESP_OK;
    s_core_ok=nvs_ok&&wifi_ok&&network_ok;
    if(monitor_ota_start()!=ESP_OK) ESP_LOGE(TAG,"OTA task unavailable");
    s_battery = xQueueCreate(1, sizeof(int));
    if (s_battery && xTaskCreate(telemetry_task, "monitor_telemetry", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "Battery telemetry unavailable");
    if (xTaskCreate(ui_task, "monitor_ui", 4096, NULL, 3, NULL) != pdPASS) ESP_LOGE(TAG, "UI task unavailable");
}
