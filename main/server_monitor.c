#include "server_monitor.h"
#include "monitor_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static server_state_t s_state;
static char s_body[SERVER_JSON_MAX + 1];
static SemaphoreHandle_t s_deadline_lock;
static int s_active_socket = -1;
static uint64_t s_deadline;
static uint64_t millis(void) { return (uint64_t)esp_timer_get_time() / 1000; }
/* An independent worker interrupts slow headers/chunks without touching TLS
 * objects. The mutex prevents shutdown of a closed/reused socket descriptor. */
static void deadline_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        xSemaphoreTake(s_deadline_lock, portMAX_DELAY);
        if (s_active_socket >= 0 && millis() >= s_deadline) {
            shutdown(s_active_socket, SHUT_RDWR);
            s_active_socket = -1;
        }
        xSemaphoreGive(s_deadline_lock);
    }
}
static void disarm_deadline(void) {
    xSemaphoreTake(s_deadline_lock, portMAX_DELAY);
    s_active_socket = -1;
    xSemaphoreGive(s_deadline_lock);
}
typedef struct { bool json, encoded; } response_meta_t;
static esp_err_t on_header(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    response_meta_t *meta = event->user_data;
    if (!strcasecmp(event->header_key, "Content-Type"))
        meta->json = !strncasecmp(event->header_value, "application/json", 16) &&
                     (event->header_value[16] == 0 || event->header_value[16] == ';');
    if (!strcasecmp(event->header_key, "Content-Encoding"))
        meta->encoded = strcasecmp(event->header_value, "identity") != 0;
    return ESP_OK;
}
static bool remaining(esp_http_client_handle_t client, uint64_t deadline) {
    uint64_t now = millis();
    return now < deadline && esp_http_client_set_timeout_ms(client, (int)(deadline - now)) == ESP_OK;
}
static bool fetch(esp_http_client_handle_t client, response_meta_t *meta, server_state_t *out, char *error, size_t error_size) {
    uint64_t begin = millis(), deadline = begin + 3000;
    memset(meta, 0, sizeof(*meta));
    snprintf(error, error_size, "HTTPS / DNS / TLS error");
    esp_http_client_set_timeout_ms(client, 3000);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) goto done;
    xSemaphoreTake(s_deadline_lock, portMAX_DELAY);
    s_active_socket = esp_http_client_get_socket(client); s_deadline = deadline;
    xSemaphoreGive(s_deadline_lock);
    if (!remaining(client, deadline)) { snprintf(error, error_size, "HTTP timeout"); goto done; }
    int64_t declared = esp_http_client_fetch_headers(client);
    if (declared < 0) goto done;
    int status = esp_http_client_get_status_code(client);
    if (status != 200) { snprintf(error, error_size, "HTTP %d", status); goto done; }
    if (!meta->json || meta->encoded) { snprintf(error, error_size, "Invalid response type"); goto done; }
    if (declared > SERVER_JSON_MAX) { snprintf(error, error_size, "Response too large"); goto done; }
    size_t used = 0;
    for (;;) {
        if (!remaining(client, deadline)) { snprintf(error, error_size, "HTTP timeout"); goto done; }
        /* Drain cached body bytes, with deadline checks between bounded reads. */
        int n = esp_http_client_read(client, s_body + used, 1);
        if (n < 0) goto done;
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            snprintf(error, error_size, "Incomplete response"); goto done;
        }
        used += (size_t)n;
        if (used > SERVER_JSON_MAX) { snprintf(error, error_size, "Response too large"); goto done; }
    }
    if (!server_state_parse(s_body, used, out)) { snprintf(error, error_size, "Invalid JSON / fields"); goto done; }
    time_t wall = time(NULL);
    if ((double)out->timestamp > (double)wall + 30 || (double)wall - (double)out->timestamp > 15) {
        snprintf(error, error_size, "Stale server timestamp"); goto done;
    }
    out->last_update = millis(); out->latency_ms = (uint32_t)(out->last_update - begin);
    disarm_deadline();
    esp_http_client_close(client);
    return true;
done:
    disarm_deadline();
    esp_http_client_close(client);
    return false;
}
void server_monitor_snapshot(server_state_t *state) {
    if (!s_lock) { memset(state, 0, sizeof(*state)); snprintf(state->error, sizeof(state->error), "Monitor unavailable"); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY); *state = s_state; xSemaphoreGive(s_lock);
    server_state_age(state, millis());
}
void server_monitor_refresh(void) { if (s_task) xTaskNotifyGive(s_task); }
static void server_monitor_task(void *arg) {
    (void)arg;
    esp_http_client_handle_t client = NULL;
    response_meta_t meta = {0};
    uint32_t revision = UINT32_MAX;
    unsigned failures = 0;
    uint64_t next = 0, last_attempt = 0;
    monitor_config_t config = {0};
    char authorization[144];
    for (;;) {
        bool manual = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0;
        uint64_t now = millis();
        if (now < next && !manual) continue;
        if (last_attempt && now - last_attempt < 1000) continue;
        last_attempt = now;
        uint32_t current = revision;
        bool ready = monitor_wifi_config(&config, &current);
        if (revision != current) {
            if (client) { esp_http_client_cleanup(client); client = NULL; }
            revision = current; failures = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY); memset(&s_state, 0, sizeof(s_state)); xSemaphoreGive(s_lock);
        }
        char error[32] = {0};
        server_state_t fresh;
        bool success = false;
        if (!ready) snprintf(error, sizeof(error), "Wi-Fi / setup pending");
        else if (time(NULL) < 1704067200) snprintf(error, sizeof(error), "Waiting for time sync");
        else {
            if (!client) {
                esp_http_client_config_t hc = {
                    .url = config.url, .method = HTTP_METHOD_GET, .timeout_ms = 3000,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .transport_type = HTTP_TRANSPORT_OVER_SSL,
                    .disable_auto_redirect = true, .max_authorization_retries = -1,
                    .buffer_size = 1024, .buffer_size_tx = 1024,
                    .event_handler = on_header, .user_data = &meta,
                };
                client = esp_http_client_init(&hc);
                if (client) {
                    snprintf(authorization, sizeof(authorization), "Bearer %s", config.token);
                    esp_err_t e = esp_http_client_set_header(client, "Authorization", authorization);
                    if (e == ESP_OK) e = esp_http_client_set_header(client, "Accept", "application/json");
                    if (e == ESP_OK) e = esp_http_client_set_header(client, "Cache-Control", "no-cache");
                    memset(authorization, 0, sizeof(authorization));
                    if (e != ESP_OK) { esp_http_client_cleanup(client); client = NULL; }
                }
            }
            if (client) success = fetch(client, &meta, &fresh, error, sizeof(error));
            else snprintf(error, sizeof(error), "HTTP allocation failed");
        }
        memset(&config, 0, sizeof(config));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (success) s_state = fresh;
        else snprintf(s_state.error, sizeof(s_state.error), "%s", error);
        server_state_age(&s_state, millis());
        xSemaphoreGive(s_lock);
        next = millis() + (success ? 5000 : server_retry_seconds(failures) * 1000);
        if (success) failures = 0; else if (failures < 4) failures++;
    }
}
esp_err_t server_monitor_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_deadline_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_deadline_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(deadline_task, "http_deadline", 2048, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    snprintf(s_state.error, sizeof(s_state.error), "Waiting for Wi-Fi");
    if (xTaskCreate(server_monitor_task, "server_monitor_task", 8192, NULL, 3, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
