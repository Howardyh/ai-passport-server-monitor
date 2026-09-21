#include "monitor_ui.h"
#include "monitor_config.h"
#include "monitor_events.h"
#include "monitor_logic.h"
#include "monitor_audio.h"
#include "monitor_ota.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static lv_obj_t *s_rows[11],*s_bars[3],*s_status,*s_host,*s_title,*s_footer,*s_qr,*s_chart;
static uint32_t s_trend[(16+MONITOR_TREND_BYTES)/4];
static char s_text[11][96],s_status_text[24],s_host_text[96],s_title_text[40],s_footer_text[96],s_qr_data[128];
static int s_page,s_section=-1,s_choice,s_diagnostic_page;
static bool s_setup,s_confirm_update;
static monitor_config_t s_config;
static monitor_history_t s_history;
static const char *pages[]={"OVERVIEW","PERFORMANCE","NETWORK","SERVICES","DEVICE","SETTINGS"};
static const char *sections[]={"Network","Audio","Display","Power","Update","Diagnostics"};
static const char *audio_names[]={"Voice","Volume","Startup Voice","Network Voice","Server Voice","Critical Alerts","Alert Sound","Critical bypass"};
static lv_obj_t *label(lv_obj_t *parent,int x,int y,int width) {
    lv_obj_t *o=lv_label_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,width);
    lv_label_set_long_mode(o,LV_LABEL_LONG_CLIP);lv_obj_set_style_text_font(o,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(o,lv_color_white(),0);return o;
}
static void row(int i,const char *fmt,...) {
    va_list args;va_start(args,fmt);vsnprintf(s_text[i],sizeof(s_text[i]),fmt,args);va_end(args);
    for(char *p=s_text[i];*p;p++) if((unsigned char)*p<32||(unsigned char)*p>=127) *p='?';
    lv_label_set_text_static(s_rows[i],s_text[i]);
}
static void visible(lv_obj_t *o,bool show) {if(show) lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);}
static void layout(void) {
    for(int i=0;i<11;i++) {lv_obj_set_pos(s_rows[i],20,76+i*18);visible(s_rows[i],true);row(i,"");}
    for(int i=0;i<3;i++) visible(s_bars[i],!s_setup&&s_page==0);
    visible(s_chart,!s_setup&&s_page==1);visible(s_qr,s_setup);
    if(s_setup) {for(int i=0;i<11;i++) visible(s_rows[i],false);visible(s_rows[0],true);lv_obj_set_y(s_rows[0],65);visible(s_rows[1],true);lv_obj_set_y(s_rows[1],252);visible(s_rows[2],true);lv_obj_set_y(s_rows[2],273);}
    else if(s_page==0) {int y[]={80,127,174,215,235,255};for(int i=0;i<6;i++) lv_obj_set_y(s_rows[i],y[i]);}
    else if(s_page==1) {for(int i=5;i<11;i++) visible(s_rows[i],false);}
}
void monitor_ui_init(void) {
    lv_obj_t *screen=lv_obj_create(NULL);lv_obj_set_style_bg_color(screen,lv_color_black(),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);lv_obj_set_style_pad_all(screen,0,0);lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    s_title=label(screen,20,20,200);s_host=label(screen,20,43,130);s_status=label(screen,144,43,80);
    lv_obj_set_style_text_color(s_title,lv_color_hex(0x91a6b0),0);
    for(int i=0;i<11;i++) s_rows[i]=label(screen,20,76+i*18,202);
    for(int i=0;i<3;i++) {s_bars[i]=lv_bar_create(screen);lv_obj_set_pos(s_bars[i],20,103+i*47);lv_obj_set_size(s_bars[i],200,7);lv_bar_set_range(s_bars[i],0,100);lv_obj_set_style_bg_color(s_bars[i],lv_color_hex(0x26313b),LV_PART_MAIN);lv_obj_set_style_radius(s_bars[i],0,LV_PART_MAIN);lv_obj_set_style_radius(s_bars[i],0,LV_PART_INDICATOR);}
    s_chart=lv_canvas_create(screen);lv_obj_set_pos(s_chart,24,187);
    lv_canvas_set_buffer(s_chart,s_trend,MONITOR_TREND_WIDTH,MONITOR_TREND_HEIGHT,LV_COLOR_FORMAT_I2);
    lv_canvas_set_palette(s_chart,0,lv_color_to_32(lv_color_black(),LV_OPA_COVER));
    lv_canvas_set_palette(s_chart,1,lv_color_to_32(lv_color_hex(0x67e6aa),LV_OPA_COVER));
    lv_canvas_set_palette(s_chart,2,lv_color_to_32(lv_color_hex(0xf5ca61),LV_OPA_COVER));
    lv_canvas_set_palette(s_chart,3,lv_color_to_32(lv_color_hex(0x303a40),LV_OPA_COVER));
    s_qr=lv_qrcode_create(screen);lv_qrcode_set_size(s_qr,156);lv_obj_set_pos(s_qr,42,91);
    lv_qrcode_set_dark_color(s_qr,lv_color_black());lv_qrcode_set_light_color(s_qr,lv_color_white());lv_qrcode_set_quiet_zone(s_qr,true);
    s_footer=label(screen,25,293,195);lv_obj_set_style_text_color(s_footer,lv_color_hex(0x91a6b0),0);
    layout();lv_screen_load(screen);
}
void monitor_ui_key(bsp_btn_t button,bsp_btn_ev_t event) {
    if(event==BSP_BTN_LONG&&button==BSP_BTN_OK) {monitor_command(CMD_SETUP,0,0);return;}
    if(event==BSP_BTN_LONG&&button==BSP_BTN_UP) {monitor_command(CMD_MUTE,0,0);return;}
    if(event==BSP_BTN_LONG&&button==BSP_BTN_DOWN) {s_page=5;s_section=-1;s_choice=0;layout();return;}
    if(event!=BSP_BTN_CLICK||s_setup) return;
    if(s_page!=5) {if(button==BSP_BTN_OK) monitor_command(CMD_REFRESH,0,0);else {s_page=(s_page+(button==BSP_BTN_DOWN?1:5))%6;s_section=-1;s_choice=0;layout();}return;}
    if(s_section<0) {
        if(button==BSP_BTN_OK) {s_section=s_choice;s_choice=0;s_confirm_update=false;}
        else {s_choice=(s_choice+(button==BSP_BTN_DOWN?1:6))%7;if(s_choice==6) {s_page=0;s_choice=0;}}
    } else if(button==BSP_BTN_UP) {s_section=-1;s_choice=0;s_confirm_update=false;}
    else if(s_section==1) {
        if(button==BSP_BTN_DOWN) s_choice=(s_choice+1)%8;
        else {
            int values[]={s_config.audio_enabled,s_config.volume,s_config.startup_voice,s_config.network_voice,s_config.server_voice,s_config.critical_alerts,s_config.alert_sound,s_config.critical_bypass_mute};
            int value=s_choice==1?(values[1]+10)%110:!values[s_choice];monitor_command(CMD_SETTING,s_choice,value);
        }
    } else if(s_section==2) {
        if(button==BSP_BTN_DOWN) s_choice=(s_choice+1)%2;
        else if(!s_choice) monitor_command(CMD_SETTING,SET_BRIGHTNESS,s_config.brightness>=100?10:s_config.brightness+10);
        else {unsigned values[]={0,30,60,300,600},i=0;while(i<4&&values[i]!=s_config.screen_timeout)i++;monitor_command(CMD_SETTING,SET_TIMEOUT,values[(i+1)%5]);}
    } else if(s_section==3&&button==BSP_BTN_OK) monitor_command(CMD_SETTING,SET_POWER,(s_config.power_mode+1)%3);
    else if(s_section==0&&button==BSP_BTN_OK) monitor_command(CMD_SETUP,0,0);
    else if(s_section==4&&button==BSP_BTN_OK) {
        monitor_ota_view_t v;monitor_ota_view(&v);
        if(!v.busy) {if(v.available&&!s_confirm_update) s_confirm_update=true;else {monitor_command(v.available?CMD_OTA_INSTALL:CMD_OTA_CHECK,0,0);s_confirm_update=false;}}
    } else if(s_section==5&&button==BSP_BTN_DOWN) s_diagnostic_page=!s_diagnostic_page;
    layout();
}
void monitor_ui_update(const server_state_t *s,const monitor_wifi_view_t *w,int battery,uint64_t now) {
    monitor_config_snapshot(&s_config);
    if(s_setup!=w->provisioning) {s_setup=w->provisioning;layout();if(!s_setup) s_qr_data[0]=0;}
    const char *mode=s->status==SERVER_STALE?"STALE":s->status==SERVER_OFFLINE?"OFFLINE":s->connection_mode==LIVE_WSS?"LIVE":s->connection_mode==HTTPS_POLLING?"POLLING":"RETRY";
    snprintf(s_status_text,sizeof(s_status_text),"%s",s_setup?"SETUP":mode);lv_label_set_text_static(s_status,s_status_text);
    lv_obj_set_style_text_color(s_status,lv_color_hex(s->online?0x67e6aa:s->status==SERVER_STALE?0xf5ca61:0xff6868),0);
    if(s_config.audio_enabled&&s_config.volume) snprintf(s_title_text,sizeof(s_title_text),"%.13s VOL %u%%",s_config.device_name,s_config.volume);else snprintf(s_title_text,sizeof(s_title_text),"%.16s MUTE",s_config.device_name);lv_label_set_text_static(s_title,s_title_text);
    snprintf(s_host_text,sizeof(s_host_text),"%s",s_setup?"SETUP":s_page==5&&s_section>=0?sections[s_section]:pages[s_page]);lv_label_set_text_static(s_host,s_host_text);
    for(int i=0;i<11;i++) row(i,"");
    char rx[24],tx[24],up[32],batt[12];monitor_format_rate(rx,sizeof(rx),s->network_rx_bps);monitor_format_rate(tx,sizeof(tx),s->network_tx_bps);monitor_format_uptime(up,sizeof(up),s->uptime);
    if(battery<0) strcpy(batt,"--");else snprintf(batt,sizeof(batt),"%d%%",battery);
    uint64_t prev=s_history.next;monitor_history_add(&s_history,s,now);
    if(s_history.next!=prev) {
        monitor_history_raster(&s_history,(uint8_t *)s_trend+16,MONITOR_TREND_BYTES);
        lv_canvas_set_draw_buf(s_chart,lv_canvas_get_draw_buf(s_chart));lv_obj_invalidate(s_chart);
    }
    if(s_setup) {
        row(0,"Scan QR to configure");row(1,"%s",w->setup_ssid);row(2,"192.168.4.1  %us",w->setup_seconds);
        char qr[128];snprintf(qr,sizeof(qr),"WIFI:T:WPA;S:%s;P:%s;;",w->setup_ssid,w->setup_key);
        if(strcmp(qr,s_qr_data)) {strcpy(s_qr_data,qr);if(lv_qrcode_update(s_qr,qr,strlen(qr))!=LV_RESULT_OK) row(0,"QR allocation failed");}
    } else if(s_page==0) {
        const char *names[]={"CPU","RAM","DISK"};float values[]={s->cpu_usage,s->memory_percent,s->disk_percent};
        for(int i=0;i<3;i++) {if(s->has_data)row(i,"%s                 %.0f%%",names[i],(double)values[i]);else row(i,"%s                 --",names[i]);lv_bar_set_value(s_bars[i],s->has_data?(int)values[i]:0,LV_ANIM_OFF);lv_obj_set_style_bg_color(s_bars[i],lv_color_hex(values[i]>=90?0xff6868:values[i]>=75?0xf5ca61:0x67e6aa),LV_PART_INDICATOR);}
        row(3,"RX %s",s->has_data?rx:"--");row(4,"TX %s",s->has_data?tx:"--");row(5,"UP %s",s->has_data?up:"--");
    } else if(s_page==1) {
        if(s->has_data) {row(0,"CPU %.1f%%   RAM %.1f%%",(double)s->cpu_usage,(double)s->memory_percent);row(2,"Load1 %.2f  Load5 %.2f",(double)s->load1,(double)s->load5);row(3,"Load15 %.2f",(double)s->load15);}else row(0,"Waiting for server");
        if(s->temperature_valid) row(1,"CPU TEMP %.1f C",(double)s->cpu_temperature);else row(1,"CPU TEMP --");row(4,"CPU green / RAM yellow");
    } else if(s_page==2) {
        row(0,"RX %s",s->has_data?rx:"--");row(1,"TX %s",s->has_data?tx:"--");if(s->latency_ms) row(3,"API latency %lu ms",(unsigned long)s->latency_ms);else row(3,"API latency --");row(4,"Wi-Fi %d dBm",w->rssi);
        row(5,"WSS %s",s->connection_mode==LIVE_WSS?"LIVE":"RECONNECTING");row(6,"RX age %llus",(unsigned long long)(s->wss_last_rx?(now-s->wss_last_rx)/1000:0));row(7,"Reconnects %lu",(unsigned long)s->reconnect_count);row(9,"%s",s->error);
    } else if(s_page==3) {
        row(0,"NGINX    %s",s->has_data?(s->service_nginx?"ONLINE":"OFFLINE"):"--");row(2,"MariaDB  %s",s->has_data?(s->service_mariadb?"ONLINE":"OFFLINE"):"--");row(4,"PHP-FPM  %s",s->has_data?(s->service_php_fpm?"ONLINE":"OFFLINE"):"--");row(7,"Last alert:");row(8,"%s",s->alert_message[0]?s->alert_message:"None");
    } else if(s_page==4) {
        row(0,"Battery %s",batt);row(1,"FW %s",esp_app_get_description()->version);monitor_format_uptime(up,sizeof(up),now/1000);row(2,"Up %s",up);row(3,"Heap %u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));row(4,"Wi-Fi %d dBm",w->rssi);row(5,"%s",w->ssid);row(6,"IP %s",w->connected?w->ip:"--");row(8,"%s",w->message);
    } else if(s_section<0) {for(int i=0;i<6;i++) row(i,"%c %s",s_choice==i?'>':' ',sections[i]);row(8,"DOWN past list: Overview");}
    else if(s_section==0) {row(0,"%s",w->ssid);row(1,"%s",w->api_host);row(2,"%s",w->message);row(4,"OK: QR provisioning");row(6,"Expires after 5 minutes");}
    else if(s_section==1) {
        unsigned values[]={s_config.audio_enabled,s_config.volume,s_config.startup_voice,s_config.network_voice,s_config.server_voice,s_config.critical_alerts,s_config.alert_sound,s_config.critical_bypass_mute};
        for(int i=0;i<8;i++) {if(i==1) row(i,"%c Volume: %u%%",s_choice==i?'>':' ',values[i]);else row(i,"%c %s: %s",s_choice==i?'>':' ',audio_names[i],values[i]?"ON":"OFF");}
        row(9,"OK: toggle / volume +10");
    } else if(s_section==2) {row(0,"%c Brightness: %u%%",s_choice==0?'>':' ',s_config.brightness);if(s_config.screen_timeout)row(1,"%c Timeout: %lus",s_choice==1?'>':' ',(unsigned long)s_config.screen_timeout);else row(1,"%c Timeout: Never",s_choice==1?'>':' ');row(4,"Any key wakes screen");}
    else if(s_section==3) {const char *m[]={"Live","Balanced","Battery Saver"};row(0,"Mode: %s",m[s_config.power_mode]);row(2,"OK: cycle power mode");row(4,"Live: WSS, screen off");row(5,"Balanced: WSS, dim");row(6,"Saver: ambient HTTP 60s");}
    else if(s_section==4) {monitor_ota_view_t v;monitor_ota_view(&v);row(0,"FW %s",esp_app_get_description()->version);row(2,"%s",v.message);row(3,"%s",v.version);row(5,"%s",s_confirm_update?"OK again: install + reboot":"OK: check / select update");row(7,"UP: cancel / back");}
    else if(!s_diagnostic_page) {
        row(0,"FW %s",esp_app_get_description()->version);row(1,"Device up %llus",(unsigned long long)(now/1000));row(2,"Heap free %u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));row(3,"Heap min %u",(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));row(4,"Largest %u",(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));row(5,"Wi-Fi %d dBm",w->rssi);row(6,"IP %s",w->connected?w->ip:"--");row(8,"DOWN: network / audio");
    } else {row(0,"WSS %s",s->connection_mode==LIVE_WSS?"LIVE":"DISCONNECTED");row(1,"WSS age %llus",(unsigned long long)(s->wss_last_rx?(now-s->wss_last_rx)/1000:0));if(s->latency_ms) row(2,"API latency %lu ms",(unsigned long)s->latency_ms);else row(2,"API latency --");row(3,"Reconnects %lu",(unsigned long)s->reconnect_count);row(4,"%s",w->api_host);row(5,"Error %s",s->error);row(6,"Audio queue %u",monitor_audio_depth());row(7,"Audio errors %u",monitor_audio_errors());row(8,"Events lost %lu",(unsigned long)monitor_events_dropped());row(9,"DOWN: memory / device");}
    if(s_setup) snprintf(s_footer_text,sizeof(s_footer_text),"Temporary WPA2 setup");
    else if(s_page==5) snprintf(s_footer_text,sizeof(s_footer_text),"%s",s_section<0?"UP/DOWN select; OK enter":"UP back | DOWN | OK");
    else if(s->status!=SERVER_ONLINE&&s->has_data) snprintf(s_footer_text,sizeof(s_footer_text),"Last update %llus ago",(unsigned long long)((now-s->last_update)/1000));
    else if(s->latency_ms) snprintf(s_footer_text,sizeof(s_footer_text),"%lums  WiFi %d  BAT%s",(unsigned long)s->latency_ms,w->rssi,batt);
    else snprintf(s_footer_text,sizeof(s_footer_text),"API --  WiFi %d  BAT%s",w->rssi,batt);
    lv_label_set_text_static(s_footer,s_footer_text);
}
