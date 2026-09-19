#include "monitor_ui.h"
#include "server_monitor.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_rows[11], *s_bars[3], *s_status, *s_host, *s_page_label, *s_footer;
static char s_text[11][96], s_status_text[12], s_host_text[96], s_footer_text[96];
static int s_page, s_choice;
static bool s_settings;
static const char *const s_pages[] = {"01 / OVERVIEW", "02 / SYSTEM", "03 / NETWORK + STORAGE", "04 / SERVICES"};
static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y); lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xffffff), 0);
    return obj;
}
static void row(int index, const char *fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(s_text[index], sizeof(s_text[index]), fmt, args); va_end(args);
    for (char *p = s_text[index]; *p; p++) if ((unsigned char)*p >= 127 || (unsigned char)*p < 32) *p = '?';
    lv_label_set_text_static(s_rows[index], s_text[index]);
}
static void rate(char *out, size_t size, double bytes) {
    const char *unit = "B/s";
    if (bytes >= 1e9) { bytes /= 1e9; unit = "GB/s"; }
    else if (bytes >= 1e6) { bytes /= 1e6; unit = "MB/s"; }
    else if (bytes >= 1e3) { bytes /= 1e3; unit = "KB/s"; }
    snprintf(out, size, "%.1f %s", bytes, unit);
}
static uint32_t metric_color(float value, int warn) { return value >= 90 ? 0xff6868 : value >= warn ? 0xf5ca61 : 0x67e6aa; }
static const char *metric_name(float value, int warn) { return value >= 90 ? "CRITICAL" : value >= warn ? "WARNING" : "NORMAL"; }
static void layout(void) {
    for (int i = 0; i < 11; i++) {
        lv_obj_set_pos(s_rows[i], 20, 89 + 18*i);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(0xffffff), 0);
        row(i, "");
    }
    for (int i = 0; i < 3; i++) {
        if (!s_settings && s_page == 0) lv_obj_remove_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_settings && s_page == 0) {
        const int y[] = {89, 136, 183, 226, 245, 264};
        for (int i = 0; i < 6; i++) lv_obj_set_y(s_rows[i], y[i]);
    }
    lv_label_set_text_static(s_page_label, s_settings ? "SETTINGS" : s_pages[s_page]);
}
void monitor_ui_init(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = label(screen, 20, 22, 125);
    lv_label_set_text_static(title, "SERVER");
    s_status = label(screen, 143, 22, 77);
    s_host = label(screen, 20, 45, 200);
    s_page_label = label(screen, 20, 67, 208);
    lv_obj_set_style_text_color(s_page_label, lv_color_hex(0x8dabbf), 0);
    for (int i = 0; i < 11; i++) s_rows[i] = label(screen, 20, 89 + 18*i, 200);
    for (int i = 0; i < 3; i++) {
        s_bars[i] = lv_bar_create(screen); lv_obj_set_pos(s_bars[i], 20, 112 + i*47); lv_obj_set_size(s_bars[i], 200, 7);
        lv_bar_set_range(s_bars[i], 0, 100);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(0x26313b), LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 0, LV_PART_INDICATOR);
    }
    s_footer = label(screen, 27, 290, 190);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x8dabbf), 0);
    layout(); lv_screen_load(screen);
}
void monitor_ui_key(bsp_btn_t button, bsp_btn_ev_t event) {
    if (button == BSP_BTN_OK && event == BSP_BTN_LONG) { s_settings = !s_settings; s_choice = 0; layout(); return; }
    if (event != BSP_BTN_CLICK) return;
    if (s_settings) {
        if (button == BSP_BTN_UP) s_choice = (s_choice + 2) % 3;
        else if (button == BSP_BTN_DOWN) s_choice = (s_choice + 1) % 3;
        else if (s_choice == 0) monitor_wifi_reconnect();
        else if (s_choice == 1) monitor_wifi_provision();
        else { s_settings = false; layout(); }
    } else {
        if (button == BSP_BTN_UP) { s_page = (s_page + 3) % 4; layout(); }
        else if (button == BSP_BTN_DOWN) { s_page = (s_page + 1) % 4; layout(); }
        else server_monitor_refresh();
    }
}
void monitor_ui_update(const server_state_t *s, const monitor_wifi_view_t *w, int battery, uint64_t now_ms) {
    snprintf(s_status_text, sizeof(s_status_text), "%s", server_status_name(s->status));
    lv_label_set_text_static(s_status, s_status_text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(s->status == SERVER_ONLINE ? 0x67e6aa : s->status == SERVER_STALE ? 0xf5ca61 : 0xff6868), 0);
    snprintf(s_host_text, sizeof(s_host_text), "%s", s->has_data ? s->hostname : w->api_host);
    lv_label_set_text_static(s_host, s_host_text);
    /* Clear reused static labels so data cannot leak between page states. */
    for (int i = 0; i < 11; i++) row(i, "");
    char batt[16], rssi[24], rx[24], tx[24];
    if (battery >= 0) snprintf(batt, sizeof(batt), "%d%%", battery); else snprintf(batt, sizeof(batt), "N/A");
    if (w->connected) snprintf(rssi, sizeof(rssi), "%d dBm", w->rssi); else snprintf(rssi, sizeof(rssi), "DISCONNECTED");
    rate(rx, sizeof(rx), s->network_rx_bps); rate(tx, sizeof(tx), s->network_tx_bps);
    if (s_settings && w->provisioning) {
        row(0, "JOIN SETUP HOTSPOT"); row(1, "%s", w->setup_ssid); row(2, "Setup key:"); row(3, "%s", w->setup_key);
        row(4, "Open 192.168.4.1"); row(5, "Expires: %us", w->setup_seconds); row(6, "Battery: %s", batt);
    } else if (s_settings) {
        row(0, "SSID: %s", w->ssid[0] ? w->ssid : "Not configured");
        row(1, "Wi-Fi: %s", rssi); row(2, "API host:"); row(3, "%s", w->api_host);
        row(4, "Firmware: %s", esp_app_get_description()->version); row(5, "Battery: %s", batt); row(6, "%s", w->message);
    } else if (s_page == 0) {
        float v[] = {s->cpu_usage, s->memory_percent, s->disk_percent};
        const char *name[] = {"CPU", "RAM", "DISK"};
        for (int i = 0; i < 3; i++) {
            int warn = i == 2 ? 80 : 70;
            if (s->has_data) row(i, "%s %.0f%% %s", name[i], (double)v[i], metric_name(v[i], warn)); else row(i, "%s --", name[i]);
            lv_bar_set_value(s_bars[i], s->has_data ? (int)v[i] : 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(s->has_data ? metric_color(v[i], warn) : 0x26313b), LV_PART_INDICATOR);
        }
        row(3, "TX %s", s->has_data ? tx : "--"); row(4, "RX %s", s->has_data ? rx : "--");
        if (s->has_data) row(5, "UPTIME %llud %02lluh", (unsigned long long)(s->uptime/86400), (unsigned long long)(s->uptime/3600%24));
        else row(5, "%s", s->error);
    } else if (!s->has_data && s_page != 3) {
        row(0, "WAITING FOR SERVER"); row(1, "%s", s->error); row(3, "%s", w->message); row(5, "Hold OK for Settings");
    } else if (s_page == 1) {
        row(0, "CPU usage     %.1f%%", (double)s->cpu_usage);
        if (s->temperature_valid) row(1, "CPU temp      %.1f C", (double)s->cpu_temperature); else row(1, "CPU temp      N/A");
        row(3, "Load 1        %.2f", (double)s->load1); row(4, "Load 5        %.2f", (double)s->load5); row(5, "Load 15       %.2f", (double)s->load15);
        row(7, "RAM used  %.2f GiB", s->memory_used/1073741824.0); row(8, "RAM total %.2f GiB", s->memory_total/1073741824.0);
        row(9, "RAM usage %.1f%%", (double)s->memory_percent);
    } else if (s_page == 2) {
        row(0, "Disk used  %.2f GiB", s->disk_used/1073741824.0); row(1, "Disk total %.2f GiB", s->disk_total/1073741824.0);
        row(2, "Disk usage %.1f%%", (double)s->disk_percent); row(4, "RX %s", rx); row(5, "TX %s", tx);
        row(7, "API %lu ms", (unsigned long)s->latency_ms); row(8, "HTTPS request latency"); row(9, "Read-only status API");
    } else if (s_page == 3) {
        row(0, "NGINX    %s", !s->has_data ? "--" : s->service_nginx ? "ONLINE" : "OFFLINE");
        row(2, "MariaDB  %s", !s->has_data ? "--" : s->service_mariadb ? "ONLINE" : "OFFLINE");
        row(4, "PHP-FPM  %s", !s->has_data ? "--" : s->service_php_fpm ? "ONLINE" : "OFFLINE");
        row(6, "SSID: %s", w->ssid[0] ? w->ssid : "Not configured"); row(7, "Wi-Fi: %s", rssi); row(9, "Battery: %s", batt);
    }
    if (s_settings) {
        row(8, "%c Reconnect Wi-Fi", s_choice == 0 ? '>' : ' ');
        row(9, "%c Wi-Fi Provisioning", s_choice == 1 ? '>' : ' ');
        row(10, "%c Back to Monitor", s_choice == 2 ? '>' : ' ');
        snprintf(s_footer_text, sizeof(s_footer_text), "UP/DOWN; OK to apply");
    } else if (s->has_data) snprintf(s_footer_text, sizeof(s_footer_text), "UPDATED %llus ago", (unsigned long long)(now_ms >= s->last_update ? (now_ms - s->last_update)/1000 : 0));
    else snprintf(s_footer_text, sizeof(s_footer_text), "Hold OK: setup | %d/4", s_page + 1);
    lv_label_set_text_static(s_footer, s_footer_text);
    if (s_settings && !w->provisioning) row(7, "%s", s->error);
    if (!s_settings && s_page != 0 && s->has_data) row(10, "%s", s->error);
}
