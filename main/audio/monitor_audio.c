#include "monitor_audio.h"
#include "monitor_events.h"
#include "monitor_config.h"
#include "monitor_logic.h"
#include "bsp_audio.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdatomic.h>
static QueueHandle_t s_queue[3];
static monitor_cooldown_t s_cooldown;
static atomic_uint s_errors;
typedef struct {uint8_t clip,priority;} audio_item_t;
static bool permitted(const monitor_config_t *c,unsigned clip,unsigned priority) {
    if(priority==2 && c->critical_bypass_mute && c->critical_alerts) return true;
    if(!c->audio_enabled||!c->volume) return false;
    if(clip==0) return c->startup_voice;
    if(clip==1||clip==2||clip==3||clip==5||clip==6||clip==7) return c->network_voice;
    if(priority==2) return c->critical_alerts;
    return c->server_voice;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;
    int clip=-1,priority=0;
    switch(id) {
        case EV_SYSTEM_BOOTED:clip=0;break;case EV_PROVISIONING_STARTED:clip=1;break;
        case EV_WIFI_CONNECTED:clip=2;break;case EV_WIFI_FAILED:clip=3;priority=1;break;
        case EV_SERVER_SYNC_FIRST_SUCCESS:clip=4;break;case EV_WSS_CONNECTED:clip=5;break;
        case EV_WSS_DISCONNECTED:clip=6;priority=1;break;case EV_HTTPS_FALLBACK_STARTED:clip=7;break;
        case EV_SERVER_RECOVERED:clip=8;break;case EV_SERVER_OFFLINE:clip=9;priority=1;break;
        case EV_OTA_STARTED:clip=10;break;case EV_OTA_SUCCESS:clip=11;break;case EV_OTA_FAILED:clip=12;priority=1;break;
        case EV_LOW_BATTERY:clip=13;priority=1;break;
        case EV_SERVER_ALERT:priority=((monitor_alert_t *)data)->level;clip=priority==2?14:priority==1?15:16;break;
        default:return;
    }
    monitor_config_t c;monitor_config_snapshot(&c);
    if(!permitted(&c,clip,priority)||!monitor_cooldown(&s_cooldown,clip,esp_timer_get_time()/1000,priority==2)) return;
    /* Event bus serializes all producers; critical messages safely discard queued lower priorities. */
    for(int i=0;i<3;i++) if(monitor_audio_preempts(priority,i)) xQueueReset(s_queue[i]);
    audio_item_t item={(uint8_t)clip,(uint8_t)priority};
    if(xQueueSend(s_queue[priority],&item,0)!=pdTRUE) atomic_fetch_add(&s_errors,1);
}
unsigned monitor_audio_depth(void) {unsigned n=0;for(int i=0;i<3;i++) if(s_queue[i]) n+=uxQueueMessagesWaiting(s_queue[i]);return n;}
unsigned monitor_audio_errors(void) {return atomic_load(&s_errors);}
static void worker(void *arg) {
    (void)arg;uint8_t encoded[256];int16_t pcm[256];
    for(;;) {
        audio_item_t item;bool got=false;
        for(int i=2;i>=0;i--) if(xQueueReceive(s_queue[i],&item,0)==pdTRUE) {got=true;break;}
        if(!got) {vTaskDelay(pdMS_TO_TICKS(50));continue;}
        monitor_config_t config;monitor_config_snapshot(&config);
        if(!permitted(&config,item.clip,item.priority)) continue;
        if(bsp_audio_set_format(16000,16,1)!=ESP_OK) {atomic_fetch_add(&s_errors,1);continue;}
        unsigned volume=config.critical_bypass_mute&&item.priority==2&&!config.volume?60:config.volume;
        bsp_audio_set_volume(volume);
        char path[48];snprintf(path,sizeof(path),"/voice/%02u.ulaw",item.clip);FILE *f=fopen(path,"rb");
        if(!f) {atomic_fetch_add(&s_errors,1);continue;}
        if(item.priority==2&&config.alert_sound) {
            for(unsigned i=0;i<256;i++) pcm[i]=(i%32<16)?2000:-2000;
            for(unsigned i=0;i<8;i++) if(bsp_audio_write(pcm,sizeof(pcm))!=ESP_OK) break;
        }
        size_t n;
        while((n=fread(encoded,1,sizeof(encoded),f))>0) {
            monitor_config_snapshot(&config);if(!permitted(&config,item.clip,item.priority)) break;
            bool preempt=false;for(unsigned p=0;p<3;p++) if(monitor_audio_preempts(p,item.priority)&&uxQueueMessagesWaiting(s_queue[p])) preempt=true;
            if(preempt) break;
            unsigned updated_volume=config.critical_bypass_mute&&item.priority==2&&!config.volume?60:config.volume;
            if(updated_volume!=volume) {volume=updated_volume;bsp_audio_set_volume(volume);}
            for(size_t i=0;i<n;i++) pcm[i]=monitor_ulaw_decode(encoded[i]);
            if(bsp_audio_write(pcm,n*sizeof(*pcm))!=ESP_OK) {atomic_fetch_add(&s_errors,1);break;}
        }
        fclose(f);
    }
}
esp_err_t monitor_audio_start(void) {
    esp_vfs_spiffs_conf_t fs={.base_path="/voice",.partition_label="voicefs",.max_files=2,.format_if_mount_failed=false};
    esp_err_t e=esp_vfs_spiffs_register(&fs);if(e!=ESP_OK) return e;
    for(int i=0;i<3;i++) {s_queue[i]=xQueueCreate(4,sizeof(audio_item_t));if(!s_queue[i]) return ESP_ERR_NO_MEM;}
    e=monitor_subscribe(ESP_EVENT_ANY_ID,event,NULL);if(e!=ESP_OK) return e;
    return xTaskCreate(worker,"audio_worker",4096,NULL,2,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
