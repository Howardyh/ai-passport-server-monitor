#include "monitor_config.h"
#include "server_state.h"
#include "nvs.h"
#include <string.h>

bool monitor_config_valid(const monitor_config_t *c) {
    if (!c || c->version != 1 || !memchr(c->ssid, 0, sizeof(c->ssid)) ||
        !memchr(c->password, 0, sizeof(c->password)) || !memchr(c->url, 0, sizeof(c->url)) ||
        !memchr(c->token, 0, sizeof(c->token))) return false;
    size_t password_len = strlen(c->password), token_len = strlen(c->token);
    if (!c->ssid[0] || password_len < 8 || password_len > 63 || token_len < 32 || token_len > 128 || !server_url_valid(c->url)) return false;
    for (const char *p = c->token; *p; p++)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || strchr("-._~+/=", *p))) return false;
    return true;
}
esp_err_t monitor_config_load(monitor_config_t *c) {
    memset(c, 0, sizeof(*c));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("srv_monitor", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*c);
    err = nvs_get_blob(handle, "config_v1", c, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof(*c) || !monitor_config_valid(c))) err = ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) memset(c, 0, sizeof(*c));
    return err;
}
esp_err_t monitor_config_save(const monitor_config_t *c) {
    if (!monitor_config_valid(c)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("srv_monitor", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "config_v1", c, sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
