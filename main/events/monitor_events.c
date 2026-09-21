#include "monitor_events.h"
#include <stdatomic.h>
ESP_EVENT_DEFINE_BASE(MONITOR_EVENTS);
static esp_event_loop_handle_t s_loop;
static atomic_uint s_dropped;
esp_err_t monitor_events_init(void) {
    esp_event_loop_args_t args = {.queue_size=24, .task_name="event_bus", .task_priority=4, .task_stack_size=3072, .task_core_id=0};
    return esp_event_loop_create(&args, &s_loop);
}
esp_err_t monitor_subscribe(int32_t id, esp_event_handler_t fn, void *arg) {
    return esp_event_handler_register_with(s_loop, MONITOR_EVENTS, id, fn, arg);
}
void monitor_emit(monitor_event_id_t id, const void *data, size_t size) {
    if (!s_loop || esp_event_post_to(s_loop, MONITOR_EVENTS, id, data, size, 0) != ESP_OK) atomic_fetch_add(&s_dropped, 1);
}
void monitor_command(monitor_command_t cmd, int setting, int value) {
    monitor_command_data_t data = {cmd, setting, value};
    monitor_emit(EV_COMMAND, &data, sizeof(data));
}
uint32_t monitor_events_dropped(void) { return atomic_load(&s_dropped); }
