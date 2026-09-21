#include "monitor_ui.h"
#include "monitor_events.h"
#include "monitor_ota.h"
#include "monitor_logic.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
static uint16_t buffer[240*20],frame[240*320];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *bytes){uint16_t *px=(uint16_t *)bytes;for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++)frame[y*240+x]=*px++;lv_display_flush_ready(d);}
void monitor_config_snapshot(monitor_config_t *c){memset(c,0,sizeof(*c));strcpy(c->device_name,"fl0AT NODE");c->audio_enabled=1;c->volume=60;c->brightness=80;c->startup_voice=c->network_voice=c->server_voice=c->critical_alerts=c->alert_sound=1;}
void monitor_command(monitor_command_t c,int s,int v){(void)c;(void)s;(void)v;}
uint32_t monitor_events_dropped(void){return 0;}
unsigned monitor_audio_depth(void){return 0;}
unsigned monitor_audio_errors(void){return 0;}
void monitor_ota_view(monitor_ota_view_t *v){memset(v,0,sizeof(*v));strcpy(v->message,"OK: check HTTPS manifest");}
const esp_app_desc_t *esp_app_get_description(void){static esp_app_desc_t a={MONITOR_TEST_VERSION};return &a;}
void host_fail(const char *file,int line){lv_mem_monitor_t m;lv_mem_monitor(&m);fprintf(stderr,"ASSERT %s:%d free=%zu largest=%zu peak=%zu\n",file,line,m.free_size,m.free_biggest_size,m.max_used);exit(3);}
static void capture(const char *name){lv_mem_monitor_t m;lv_mem_monitor(&m);fprintf(stderr,"Capture %s free=%zu largest=%zu peak=%zu\n",name,m.free_size,m.free_biggest_size,m.max_used);lv_obj_invalidate(lv_screen_active());for(int i=0;i<4;i++){lv_tick_inc(40);lv_timer_handler();}char path[80];snprintf(path,sizeof(path),"%s.ppm",name);FILE*f=fopen(path,"wb");fprintf(f,"P6\n240 320\n255\n");for(int i=0;i<240*320;i++){unsigned v=frame[i];unsigned char rgb[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,f);}fclose(f);}
int main(void){lv_init();lv_display_t*d=lv_display_create(240,320);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);monitor_ui_init();
server_state_t s={.has_data=true,.online=true,.status=SERVER_ONLINE,.connection_mode=LIVE_WSS,.last_update=10000,.temperature_valid=true,.cpu_temperature=51.2,.load1=0.53,.load5=0.38,.load15=0.29,.cpu_usage=27,.memory_percent=48,.disk_percent=42,.network_rx_bps=12400,.network_tx_bps=2100,.uptime=99999,.latency_ms=42,.service_nginx=true,.service_mariadb=true,.service_php_fpm=true};monitor_wifi_view_t w={.connected=true,.rssi=-51};strcpy(w.ssid,"Demo Wi-Fi");strcpy(w.ip,"192.168.1.42");strcpy(w.api_host,"status.example.com");
for(unsigned point=0;point<600;point++){s.cpu_usage=15+point%60;s.memory_percent=40+point%20;monitor_ui_update(&s,&w,83,point*2000);if(point%30==0){lv_tick_inc(40);lv_timer_handler();}}
s.cpu_usage=27;s.memory_percent=48;
for(int page=0;page<6;page++){monitor_ui_update(&s,&w,83,10000);char name[30];snprintf(name,sizeof(name),"page-%d",page);capture(name);monitor_ui_key(BSP_BTN_DOWN,BSP_BTN_CLICK);}
monitor_ui_key(BSP_BTN_DOWN,BSP_BTN_LONG);monitor_ui_key(BSP_BTN_DOWN,BSP_BTN_CLICK);monitor_ui_key(BSP_BTN_OK,BSP_BTN_CLICK);monitor_ui_update(&s,&w,83,10000);capture("audio");
w.provisioning=true;strcpy(w.setup_ssid,"fl0AT-Passport-A31F");strcpy(w.setup_key,"example-only-key");w.setup_seconds=280;monitor_ui_update(&s,&w,83,10000);capture("setup");
lv_mem_monitor_t mem;lv_mem_monitor(&mem);printf("Host LVGL used=%zu max=%zu (64-bit host, 32KiB pool, BSP-sized partial buffer; not device measurement)\n",mem.total_size-mem.free_size,mem.max_used);return 0;}
