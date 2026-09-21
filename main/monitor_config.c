#include "monitor_config.h"
#include "monitor_logic.h"
#include "monitor_events.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
static SemaphoreHandle_t s_lock, s_save_lock;
static monitor_config_t s_config;
static bool s_nvs_ok;
void monitor_config_defaults(monitor_config_t *c) {
    memset(c,0,sizeof(*c)); c->version=2;
    strcpy(c->url,"https://status.dyhcn.com/api/v1/status");
    strcpy(c->ws_url,"wss://status.dyhcn.com/ws");
    strcpy(c->host,"status.dyhcn.com"); strcpy(c->server_name,"server");
    strcpy(c->device_name,"fl0AT NODE"); strcpy(c->timezone,"CST-8");
    c->audio_enabled=1;c->volume=60;c->startup_voice=1;c->network_voice=1;c->server_voice=1;
    c->critical_alerts=1;c->alert_sound=1;c->brightness=80;
}
static bool settings_valid(const monitor_config_t *c) {
    return c->volume<=100 && c->brightness>=10 && c->brightness<=100 && c->power_mode<=2 &&
        (c->screen_timeout==0||c->screen_timeout==30||c->screen_timeout==60||c->screen_timeout==300||c->screen_timeout==600)&&
        c->audio_enabled<=1&&c->startup_voice<=1&&c->network_voice<=1&&c->server_voice<=1&&
        c->critical_alerts<=1&&c->alert_sound<=1&&c->critical_bypass_mute<=1;
}
static bool strings_valid(const monitor_config_t *c) {
#define TERMINATED(f) if(!memchr(c->f,0,sizeof(c->f))) return false
    TERMINATED(ssid); TERMINATED(password); TERMINATED(url); TERMINATED(token); TERMINATED(ws_url);
    TERMINATED(server_name); TERMINATED(host); TERMINATED(device_name); TERMINATED(timezone);
#undef TERMINATED
    return true;
}
bool monitor_config_valid(const monitor_config_t *c) {
    if(!c||c->version!=2||!strings_valid(c)||!settings_valid(c)) return false;
    size_t pw=strlen(c->password), token=strlen(c->token);
    if(!c->ssid[0]||pw<8||pw>63||token<32||token>128||!server_url_valid(c->url)||!monitor_wss_url_valid(c->ws_url)) return false;
    for(const char *p=c->token;*p;p++) if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-._~+/=",*p))) return false;
    for(const char *p=c->timezone;*p;p++) if((unsigned char)*p<32||(unsigned char)*p>=127) return false;
    return c->device_name[0]&&c->host[0];
}
static void publish(const monitor_config_t *c) {
    xSemaphoreTake(s_lock,portMAX_DELAY);s_config=*c;xSemaphoreGive(s_lock);
    setenv("TZ",c->timezone,1);tzset();
}
esp_err_t monitor_config_load(monitor_config_t *c) {
    monitor_config_defaults(c);nvs_handle_t h;
    esp_err_t e=nvs_open("srv_monitor",NVS_READONLY,&h);if(e!=ESP_OK) return e;
    monitor_config_t stored;size_t n=sizeof(stored);e=nvs_get_blob(h,"config_v2",&stored,&n);
    if(e==ESP_OK && n==sizeof(stored) && stored.version==2 && strings_valid(&stored) && settings_valid(&stored)) *c=stored;
    else if(e==ESP_ERR_NVS_NOT_FOUND) {
        /* Migrate the old HTTPS configuration without erasing any NVS namespace. */
        struct {uint32_t version;char ssid[33],password[65],url[256],token[129];} old;
        n=sizeof(old);e=nvs_get_blob(h,"config_v1",&old,&n);
        if(e==ESP_OK && n==sizeof(old) && old.version==1 && memchr(old.ssid,0,sizeof(old.ssid)) &&
           memchr(old.password,0,sizeof(old.password)) && memchr(old.url,0,sizeof(old.url)) && memchr(old.token,0,sizeof(old.token)) && server_url_valid(old.url)) {
            strcpy(c->ssid,old.ssid);strcpy(c->password,old.password);strcpy(c->url,old.url);strcpy(c->token,old.token);
            const char *slash=strchr(old.url+8,'/');size_t host_len=(size_t)(slash-(old.url+8));
            if(host_len<sizeof(c->host)) {
                memcpy(c->host,old.url+8,host_len);c->host[host_len]=0;
                snprintf(c->ws_url,sizeof(c->ws_url),"wss://%s/ws",c->host);
            } else {monitor_config_defaults(c);e=ESP_ERR_INVALID_ARG;}
        }
        memset(&old,0,sizeof(old));
    } else e=ESP_ERR_INVALID_ARG;
    nvs_close(h);memset(&stored,0,sizeof(stored));return e;
}
esp_err_t monitor_config_init(void) {
    s_lock=xSemaphoreCreateMutex();s_save_lock=xSemaphoreCreateMutex();if(!s_lock||!s_save_lock) return ESP_ERR_NO_MEM;
    monitor_config_defaults(&s_config);esp_err_t e=nvs_flash_init();s_nvs_ok=e==ESP_OK;
    if(s_nvs_ok) {monitor_config_t c;monitor_config_load(&c);publish(&c);memset(&c,0,sizeof(c));}
    return e;
}
void monitor_config_snapshot(monitor_config_t *c) {
    if(!s_lock) {monitor_config_defaults(c);return;}
    xSemaphoreTake(s_lock,portMAX_DELAY);*c=s_config;xSemaphoreGive(s_lock);
}
esp_err_t monitor_config_save(const monitor_config_t *c) {
    if(!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    if(!c||c->version!=2||!strings_valid(c)||!settings_valid(c)|| (c->ssid[0]&&!monitor_config_valid(c))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_save_lock,portMAX_DELAY);
    nvs_handle_t h;esp_err_t e=nvs_open("srv_monitor",NVS_READWRITE,&h);if(e!=ESP_OK) {xSemaphoreGive(s_save_lock);return e;}
    e=nvs_set_blob(h,"config_v2",c,sizeof(*c));if(e==ESP_OK) e=nvs_commit(h);nvs_close(h);
    if(e==ESP_OK) {publish(c);monitor_emit(EV_CONFIG_CHANGED,NULL,0);}
    xSemaphoreGive(s_save_lock);return e;
}
esp_err_t monitor_config_setting(monitor_setting_t setting,int value) {
    if(setting==SET_VOLUME) value=(int)monitor_volume(value);
    else if(value<0 || (setting<=SET_BRIGHTNESS&&value>100)) return ESP_ERR_INVALID_ARG;
    monitor_config_t c;monitor_config_snapshot(&c);
    uint8_t *fields[]={&c.audio_enabled,&c.volume,&c.startup_voice,&c.network_voice,&c.server_voice,&c.critical_alerts,&c.alert_sound,&c.critical_bypass_mute,&c.brightness};
    if(setting>=SET_VOICE&&setting<=SET_BRIGHTNESS) *fields[setting]=(uint8_t)value;
    else if(setting==SET_TIMEOUT) c.screen_timeout=(uint32_t)value;
    else if(setting==SET_POWER) c.power_mode=(uint8_t)value;
    else return ESP_ERR_INVALID_ARG;
    esp_err_t e=monitor_config_save(&c);memset(&c,0,sizeof(c));return e;
}
