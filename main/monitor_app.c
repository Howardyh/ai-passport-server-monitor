#include "bsp_battery.h"
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
static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *arg) {
    (void)arg;
    input_t input = {.button = button, .event = event};
    if (s_input) (void)xQueueSend(s_input, &input, 0);
}
static void telemetry_task(void *arg) {
    (void)arg;
    bool battery_ok = bsp_battery_init() == ESP_OK;
    uint64_t next_log = 0;
    for (;;) {
        int battery = battery_ok ? bsp_battery_soc() : -1;
        xQueueOverwrite(s_battery, &battery);
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
    for (;;) {
        input_t input;
        bool key = xQueueReceive(s_input, &input, pdMS_TO_TICKS(100)) == pdTRUE;
        uint64_t now = (uint64_t)esp_timer_get_time()/1000;
        if (s_battery) (void)xQueueReceive(s_battery, &battery, 0);
        if (key || now >= next_ui) {
            server_state_t state; monitor_wifi_view_t wifi;
            server_monitor_snapshot(&state); monitor_wifi_view(&wifi);
            if (bsp_lvgl_lock(50)) {
                if (key) monitor_ui_key(input.button, input.event);
                monitor_ui_update(&state, &wifi, battery, now);
                bsp_lvgl_unlock();
            }
            next_ui = now + 1000;
        }
    }
}
void app_main(void) {
    ESP_LOGI(TAG, "AI Passport Server Monitor starting");
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) { ESP_LOGE(TAG, "Display unavailable"); return; }
    if (!bsp_lvgl_lock(1000)) return;
    monitor_ui_init(); bsp_lvgl_unlock(); bsp_display_backlight(80);
    s_input = xQueueCreate(12, sizeof(input_t));
    if (!s_input) { ESP_LOGE(TAG, "Input queue unavailable"); return; }
    if (bsp_button_init(on_button, NULL) != ESP_OK) ESP_LOGE(TAG, "Buttons unavailable");
    if (monitor_wifi_start() != ESP_OK) ESP_LOGE(TAG, "Wi-Fi initialization failed; no erase or reboot");
    if (server_monitor_start() != ESP_OK) ESP_LOGE(TAG, "HTTPS task unavailable");
    s_battery = xQueueCreate(1, sizeof(int));
    if (s_battery && xTaskCreate(telemetry_task, "monitor_telemetry", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "Battery telemetry unavailable");
    if (xTaskCreate(ui_task, "monitor_ui", 4096, NULL, 3, NULL) != pdPASS) ESP_LOGE(TAG, "UI task unavailable");
}
