#include "monitor_wifi.h"
#include "monitor_events.h"
#include "esp_mac.h"
#include "captive_dns.h"
#include "server_state.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOT_IP BIT0
#define LOST_IP BIT1
typedef enum { WIFI_CMD_RECONNECT, WIFI_CMD_PROVISION, WIFI_CMD_CONFIG } command_type_t;
typedef struct { command_type_t type; monitor_config_t config; } command_t;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;
static EventGroupHandle_t s_events;
static monitor_wifi_view_t s_view = {.rssi = -127};
static httpd_handle_t s_httpd;
static char s_nonce[33];
static bool s_sntp_ok;
static char s_networks[1600]="[]";
static char s_page[6000];
static uint64_t millis(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void message(const char *s) { lock(); snprintf(s_view.message, sizeof(s_view.message), "%s", s); unlock(); }
void monitor_wifi_view(monitor_wifi_view_t *v) {
    if (!s_lock) { memset(v, 0, sizeof(*v)); v->rssi = -127; snprintf(v->message, sizeof(v->message), "Wi-Fi unavailable"); return; }
    lock(); *v = s_view; unlock();
}
bool monitor_wifi_config(monitor_config_t *c, uint32_t *revision) {
    if (!s_lock) return false;
    monitor_config_snapshot(c);
    lock();
    bool ready = s_view.connected && !s_view.provisioning && monitor_config_valid(c);
    *revision = s_view.revision;
    unlock();
    return ready;
}
void monitor_wifi_reconnect(void) {
    const command_t cmd = {.type = WIFI_CMD_RECONNECT};
    if (s_commands) (void)xQueueSend(s_commands, &cmd, 0);
}
void monitor_wifi_provision(void) {
    const command_t cmd = {.type = WIFI_CMD_PROVISION};
    if (s_commands) (void)xQueueSend(s_commands, &cmd, 0);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_events, LOST_IP);
        xEventGroupSetBits(s_events, GOT_IP);
    } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        xEventGroupClearBits(s_events, GOT_IP);
        xEventGroupSetBits(s_events, LOST_IP);
    }
}
static void random_hex(char *out, unsigned bytes) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < bytes; i++) { unsigned v = esp_random(); out[2*i] = hex[v & 15]; out[2*i+1] = hex[(v >> 4) & 15]; }
    out[bytes*2] = 0;
}
static esp_err_t portal_page(httpd_req_t *req) {
    char *page=s_page;
    snprintf(page, sizeof(s_page),
        "<!doctype html><meta name=viewport content='width=device-width'><title>AI Passport Setup</title>"
        "<style>body{max-width:32em;margin:2em auto;font:18px system-ui;background:#10151b;color:#fff}"
        "input,button{display:block;box-sizing:border-box;width:100%%;padding:12px;margin:10px 0}</style>"
        "<h1>AI Passport Setup</h1><h2>Wi-Fi</h2><label>Available Networks<select id=n><option>Select or enter SSID</option></select></label><p>Saved credentials are never displayed. Setup expires after 5 minutes.</p>"
        "<form id=f><input list=networks name=ssid placeholder='2.4 GHz Wi-Fi SSID' maxlength=32 required>"
        "<input name=password type=password placeholder='Wi-Fi password' minlength=8 maxlength=63 required autocomplete=new-password>"
        "<h2>Server</h2><label>Host<input name=host value='status.dyhcn.com' maxlength=95 required></label><label>WebSocket URL<input name=ws_url value='wss://status.dyhcn.com/ws' maxlength=253 required></label><label>HTTPS API URL<input name=url type=url value='https://status.dyhcn.com/api/v1/status' maxlength=255 required></label>"
        "<input name=token type=password placeholder='API bearer token (32-128 characters)' minlength=32 maxlength=128 required autocomplete=new-password>"
        "<h2>Device</h2><label>Device Name<input name=device_name value='fl0AT NODE' maxlength=32 required></label><label>Timezone (POSIX)<input name=timezone value='CST-8' maxlength=63 required></label><h2>Audio</h2><label>Default Volume<input name=volume type=number min=0 max=100 value=60 required></label><button>Save &amp; Connect</button></form><p id=result></p><script>"
        "fetch('/networks').then(r=>r.json()).then(a=>a.forEach(s=>{let o=document.createElement('option');o.textContent=s;o.value=s;n.appendChild(o)}));n.onchange=()=>f.elements.ssid.value=n.value;f.onsubmit=async e=>{e.preventDefault();const b=f.querySelector('button');b.disabled=true;"
        "try{const r=await fetch('/configure',{method:'POST',headers:{'Content-Type':'application/json',"
        "'X-Passport-Setup':'%s'},body:JSON.stringify(Object.fromEntries(new FormData(f)))});"
        "result.textContent=await r.text();if(r.ok)f.reset();}catch(e){result.textContent='Check device screen';}b.disabled=false;};</script>", s_nonce);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'");
    return httpd_resp_sendstr(req, page);
}
static bool copy_json_string(cJSON *root, const char *key, char *dst, size_t size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= size) return false;
    memcpy(dst, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}
static esp_err_t portal_config(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char nonce[40];
    if (httpd_req_get_hdr_value_str(req, "X-Passport-Setup", nonce, sizeof(nonce)) != ESP_OK || strcmp(nonce, s_nonce))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid setup session");
    if (req->content_len == 0 || req->content_len > 1536)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
    char body[1537];
    size_t count = 0;
    uint64_t deadline = millis() + 3000;
    while (count < req->content_len && millis() < deadline) {
        int got = httpd_req_recv(req, body + count, req->content_len - count);
        if (got <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request");
        count += (size_t)got;
    }
    if (count != req->content_len) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request timed out");
    body[count] = 0;
    /* The form has no nested values. Reject nesting before cJSON recursion. */
    unsigned depth = 0; bool quoted = false, escape = false, valid = true;
    for (size_t i = 0; i < count; i++) {
        char ch = body[i];
        if (quoted) { if (escape) escape = false; else if (ch == '\\') escape = true; else if (ch == '"') quoted = false; }
        else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 2) valid = false; }
        else if (ch == '}' || ch == ']') { if (!depth) valid = false; else depth--; }
    }
    cJSON *root = valid && !quoted && !depth ? cJSON_ParseWithLengthOpts(body, count + 1, NULL, true) : NULL;
    command_t cmd = {.type = WIFI_CMD_CONFIG};
    monitor_config_snapshot(&cmd.config);
    bool ok = root && cJSON_IsObject(root) &&
        copy_json_string(root, "ssid", cmd.config.ssid, sizeof(cmd.config.ssid)) &&
        copy_json_string(root, "password", cmd.config.password, sizeof(cmd.config.password)) &&
        copy_json_string(root, "url", cmd.config.url, sizeof(cmd.config.url)) &&
        copy_json_string(root, "token", cmd.config.token, sizeof(cmd.config.token)) &&
        copy_json_string(root, "ws_url", cmd.config.ws_url, sizeof(cmd.config.ws_url)) &&
        copy_json_string(root, "host", cmd.config.host, sizeof(cmd.config.host)) &&
        copy_json_string(root, "device_name", cmd.config.device_name, sizeof(cmd.config.device_name)) &&
        copy_json_string(root, "timezone", cmd.config.timezone, sizeof(cmd.config.timezone));
    char volume[5];
    ok=ok&&copy_json_string(root,"volume",volume,sizeof(volume));
    if(ok) {char *end; long v=strtol(volume,&end,10);ok=*end==0&&v>=0&&v<=100;cmd.config.volume=(uint8_t)v;}
    ok=ok&&monitor_config_valid(&cmd.config);
    if (root) cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid fields: HTTPS URL, WPA2 password, 32-128 character token required");
    bool queued = xQueueSend(s_commands, &cmd, 0) == pdTRUE;
    memset(&cmd, 0, sizeof(cmd));
    if (!queued) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Busy; retry shortly");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Configuration received. Check device for saved/failed status.");
}
static void portal_stop(void) {
    captive_dns_enable(false);
    xEventGroupClearBits(s_events,GOT_IP|LOST_IP);
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    memset(s_nonce, 0, sizeof(s_nonce));
    lock(); s_view.provisioning = false; s_view.setup_key[0] = 0; s_view.setup_ssid[0] = 0; unlock();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
}
static esp_err_t portal_networks(httpd_req_t *req) {
    httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,s_networks);
}
static void scan_networks(void) {
    wifi_scan_config_t scan={.show_hidden=false};
    if(esp_wifi_scan_start(&scan,true)!=ESP_OK) return;
    wifi_ap_record_t aps[12];uint16_t count=12;
    if(esp_wifi_scan_get_ap_records(&count,aps)!=ESP_OK) return;
    cJSON *list=cJSON_CreateArray();if(!list) return;
    for(unsigned i=0;i<count;i++) {aps[i].ssid[32]=0;cJSON_AddItemToArray(list,cJSON_CreateString((char *)aps[i].ssid));}
    char *json=cJSON_PrintUnformatted(list);
    if(json) {snprintf(s_networks,sizeof(s_networks),"%s",json);cJSON_free(json);}cJSON_Delete(list);
}
static esp_err_t portal_start(void) {
    (void)esp_wifi_disconnect();xEventGroupClearBits(s_events,GOT_IP|LOST_IP);
    scan_networks();
    wifi_config_t ap = {0};
    char key[17], name[33];
    random_hex(key, 8); random_hex(s_nonce, 16);
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);
    snprintf(name, sizeof(name), "fl0AT-Passport-%02X%02X",mac[4],mac[5]);
    memcpy(ap.ap.ssid, name, strlen(name)); ap.ap.ssid_len = strlen(name);
    memcpy(ap.ap.password, key, strlen(key));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.max_connection = 1; ap.ap.channel = 1;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) { portal_stop(); return err; }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192; hc.max_open_sockets = 2; hc.lru_purge_enable = true;
    hc.uri_match_fn=httpd_uri_match_wildcard;hc.max_uri_handlers=5;
    hc.recv_wait_timeout = 2; hc.send_wait_timeout = 2;
    err = httpd_start(&s_httpd, &hc);
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = portal_page};
    httpd_uri_t save = {.uri = "/configure", .method = HTTP_POST, .handler = portal_config};
    if (err == ESP_OK) err = httpd_register_uri_handler(s_httpd, &page);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_httpd, &save);
    if(err==ESP_OK) { httpd_uri_t networks={.uri="/networks",.method=HTTP_GET,.handler=portal_networks};err=httpd_register_uri_handler(s_httpd,&networks); }
    if(err==ESP_OK) { httpd_uri_t captive={.uri="/*",.method=HTTP_GET,.handler=portal_page};err=httpd_register_uri_handler(s_httpd,&captive); }
    if (err != ESP_OK) { portal_stop(); return err; }
    captive_dns_enable(true);monitor_emit(EV_PROVISIONING_STARTED,NULL,0);
    lock(); s_view.connected = false; s_view.provisioning = true;
    snprintf(s_view.setup_ssid, sizeof(s_view.setup_ssid), "%s", name);
    snprintf(s_view.setup_key, sizeof(s_view.setup_key), "%s", key); unlock();
    memset(&ap, 0, sizeof(ap)); memset(key, 0, sizeof(key));
    message("Setup active; expires in 5 min");
    return ESP_OK;
}
static bool connect_station(void) {
    wifi_config_t wc = {0};
    monitor_config_t cfg;monitor_config_snapshot(&cfg);
    if (!monitor_config_valid(&cfg)) { message("Hold OK for Settings / setup"); return false; }
    memcpy(wc.sta.ssid, cfg.ssid, strlen(cfg.ssid));
    memcpy(wc.sta.password, cfg.password, strlen(cfg.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    memset(&wc, 0, sizeof(wc)); memset(&cfg, 0, sizeof(cfg));
    message(err == ESP_OK ? "Connecting Wi-Fi" : "Wi-Fi retry pending");
    monitor_emit(err==ESP_OK?EV_WIFI_CONNECTING:EV_WIFI_FAILED,NULL,0);
    return err == ESP_OK;
}
static void publish_config(const monitor_config_t *cfg) {
    lock(); s_view.revision++;
    snprintf(s_view.ssid, sizeof(s_view.ssid), "%s", cfg->ssid);
    if (server_url_valid(cfg->url)) {
        size_t n = strcspn(cfg->url + 8, "/");
        snprintf(s_view.api_host, sizeof(s_view.api_host), "%.*s", (int)n, cfg->url + 8);
    } else snprintf(s_view.api_host, sizeof(s_view.api_host), "API not configured");
    unlock();
}
static void wifi_task(void *arg) {
    (void)arg;
    /* Owned only by this singleton worker. Reuse the command's config storage
     * at startup; two 1 KiB configs on this stack left too little headroom for
     * newlib formatting and the Wi-Fi driver on ESP32-C3. */
    static command_t cmd;
    monitor_config_snapshot(&cmd.config);
    bool configured=monitor_config_valid(&cmd.config);
    publish_config(&cmd.config); memset(&cmd, 0, sizeof(cmd));
    unsigned failures = 0;
    uint64_t next = 0, connect_deadline = 0, setup_deadline = 0;
    bool connecting = false, was_connected = false;
    if(!configured && portal_start()==ESP_OK) setup_deadline=millis()+300000;
    for (;;) {
        if (xQueueReceive(s_commands, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cmd.type == WIFI_CMD_PROVISION) {
                if (s_httpd) portal_stop();
                if (portal_start() == ESP_OK) setup_deadline = millis() + 300000;
                else message("Setup failed; retry in Settings");
                connecting = false;
            } else if (cmd.type == WIFI_CMD_CONFIG) {
                if (monitor_config_save(&cmd.config) == ESP_OK) {
                    publish_config(&cmd.config);
                    portal_stop(); message("Saved; reconnecting"); next = 0; connecting = false;
                } else message("NVS save failed; settings kept");
            } else {
                if (s_httpd) portal_stop();
                (void)esp_wifi_disconnect(); next = millis() + 2000; connecting = false;
            }
            memset(&cmd, 0, sizeof(cmd));
        }
        uint64_t now = millis();
        if (s_httpd) {
            if (now >= setup_deadline) { portal_stop(); message("Setup expired"); next = now + 2000; }
            else { lock(); s_view.setup_seconds = (unsigned)((setup_deadline - now) / 1000); unlock(); continue; }
        }
        EventBits_t bits = xEventGroupGetBits(s_events);
        bool connected = (bits & GOT_IP) != 0;
        lock(); s_view.connected = connected; unlock();
        if (connected) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { lock(); s_view.rssi = ap.rssi; unlock(); }
            if (!was_connected) { monitor_emit(EV_WIFI_CONNECTED,NULL,0);message("Wi-Fi connected");
                esp_netif_ip_info_t ip;if(esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),&ip)==ESP_OK) {lock();snprintf(s_view.ip,sizeof(s_view.ip),IPSTR,IP2STR(&ip.ip));unlock();} if (s_sntp_ok) esp_netif_sntp_start(); }
            failures = 0; connecting = false;
        } else {
            lock(); s_view.rssi = -127; unlock();
            if (bits & LOST_IP) {
                xEventGroupClearBits(s_events, LOST_IP);
                next = now + wifi_retry_seconds(failures) * 1000;
                if (failures < 4) failures++;
                connecting = false;
                message("Wi-Fi disconnected; retrying");
                if(was_connected) monitor_emit(EV_WIFI_DISCONNECTED,NULL,0);
            }
            if (connecting && now >= connect_deadline) {
                monitor_emit(EV_WIFI_FAILED,NULL,0);
                (void)esp_wifi_disconnect(); connecting = false;
                next = now + wifi_retry_seconds(failures) * 1000;
                if (failures < 4) failures++;
            }
            if (!connecting && now >= next) {
                connecting = connect_station(); connect_deadline = now + 20000;
                next = now + wifi_retry_seconds(failures) * 1000;
                if (!connecting && failures < 4) failures++;
            }
        }
        was_connected = connected;
    }
}
static void wifi_command(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;(void)id;
    if(((monitor_command_data_t *)data)->command==CMD_SETUP) monitor_wifi_provision();
}
esp_err_t monitor_wifi_start(void) {
    s_lock = xSemaphoreCreateMutex(); s_commands = xQueueCreate(2, sizeof(command_t)); s_events = xEventGroupCreate();
    if (!s_lock || !s_commands || !s_events) return ESP_ERR_NO_MEM;
    /* Preserve NVS on all errors: no erase or reboot recovery. */
    /* NVS is owned by the configuration center. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta() || !esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    wc.nvs_enable = false; /* Application owns the one persistent config blob. */
    if ((err = esp_wifi_init(&wc)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ntp.start = false;
    s_sntp_ok = esp_netif_sntp_init(&ntp) == ESP_OK;
    if(captive_dns_start()!=ESP_OK) return ESP_FAIL;
    if(monitor_subscribe(EV_COMMAND,wifi_command,NULL)!=ESP_OK) return ESP_FAIL;
    if (xTaskCreate(wifi_task, "monitor_wifi", 5120, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
