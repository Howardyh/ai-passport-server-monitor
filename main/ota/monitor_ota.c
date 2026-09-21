#include "monitor_ota.h"
#include "monitor_config.h"
#include "monitor_logic.h"
#include "monitor_events.h"
#include "server_monitor.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static monitor_ota_view_t s_view;
static monitor_manifest_t s_manifest;
static atomic_bool s_checked;
static uint64_t millis(void) {return esp_timer_get_time()/1000;}
static void message(const char *text,bool busy) {
    xSemaphoreTake(s_lock,portMAX_DELAY);snprintf(s_view.message,sizeof(s_view.message),"%s",text);s_view.busy=busy;xSemaphoreGive(s_lock);
}
void monitor_ota_view(monitor_ota_view_t *v) {
    if(!s_lock) {memset(v,0,sizeof(*v));strcpy(v->message,"OTA unavailable");return;}
    xSemaphoreTake(s_lock,portMAX_DELAY);*v=s_view;xSemaphoreGive(s_lock);
}
static bool same_origin(const char *a,const char *b) {
    if(!server_url_valid(a)||!server_url_valid(b)) return false;
    size_t an=strcspn(a+8,"/"),bn=strcspn(b+8,"/");return an==bn&&!strncmp(a+8,b+8,an);
}
static esp_http_client_handle_t open_https(const char *url,const char *token) {
    esp_http_client_config_t cfg={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=3000,
        .transport_type=HTTP_TRANSPORT_OVER_SSL,.disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=1024};
    esp_http_client_handle_t c=esp_http_client_init(&cfg);if(!c) return NULL;
    char header[144];snprintf(header,sizeof(header),"Bearer %s",token);
    esp_err_t e=esp_http_client_set_header(c,"Authorization",header);memset(header,0,sizeof(header));
    if(e!=ESP_OK||esp_http_client_open(c,0)!=ESP_OK||esp_http_client_fetch_headers(c)<0||esp_http_client_get_status_code(c)!=200) {
        esp_http_client_cleanup(c);return NULL;
    }
    return c;
}
static bool check(const monitor_config_t *cfg) {
    xSemaphoreTake(s_lock,portMAX_DELAY);s_view.available=false;xSemaphoreGive(s_lock);
    char url[256],body[1025];size_t host=strcspn(cfg->url+8,"/");
    snprintf(url,sizeof(url),"https://%.*s/firmware/manifest.json",(int)host,cfg->url+8);
    esp_http_client_handle_t c=open_https(url,cfg->token);if(!c) return false;
    int declared=esp_http_client_get_content_length(c),used=0;uint64_t deadline=millis()+5000;
    bool ok=declared<=1024;
    while(ok&&millis()<deadline&&used<1025) {
        int n=esp_http_client_read(c,body+used,1025-used);
        if(n<0) {ok=false;break;}
        if(!n) {ok=esp_http_client_is_complete_data_received(c);break;}
        used+=n;
    }
    ok=ok&&millis()<deadline&&used<=1024&&monitor_manifest_parse(body,used,&s_manifest)&&same_origin(cfg->url,s_manifest.url);
    esp_http_client_cleanup(c);
    if(ok) {
        bool newer=monitor_version_compare(s_manifest.version,esp_app_get_description()->version)>0;
        xSemaphoreTake(s_lock,portMAX_DELAY);s_view.available=newer;strcpy(s_view.version,s_manifest.version);xSemaphoreGive(s_lock);
        if(newer) monitor_emit(EV_OTA_AVAILABLE,NULL,0);
        message(newer?"Update available; OK to install":"Already up to date",false);
    }
    return ok;
}
static bool install(const monitor_config_t *cfg) {
    if(!same_origin(cfg->url,s_manifest.url)||monitor_version_compare(s_manifest.version,esp_app_get_description()->version)<=0) return false;
    const esp_partition_t *slot=esp_ota_get_next_update_partition(NULL);
    if(!slot||s_manifest.size>slot->size) return false;
    esp_http_client_handle_t c=open_https(s_manifest.url,cfg->token);if(!c) return false;
    if(esp_http_client_get_content_length(c)!=(int)s_manifest.size) {esp_http_client_cleanup(c);return false;}
    esp_ota_handle_t handle=0;bool opened=false,ok=false;
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    uint8_t chunk[1024],digest[32];uint32_t received=0;uint64_t deadline=millis()+120000;
    if(mbedtls_sha256_starts(&hash,0)!=0||esp_ota_begin(slot,s_manifest.size,&handle)!=ESP_OK) goto done;
    opened=true;
    while(received<s_manifest.size && millis()<deadline) {
        unsigned want=s_manifest.size-received;if(want>sizeof(chunk)) want=sizeof(chunk);
        int n=esp_http_client_read(c,(char *)chunk,want);if(n<=0) goto done;
        if(mbedtls_sha256_update(&hash,chunk,n)!=0||esp_ota_write(handle,chunk,n)!=ESP_OK) goto done;
        received+=n;
    }
    if(received!=s_manifest.size||millis()>=deadline||mbedtls_sha256_finish(&hash,digest)!=0||memcmp(digest,s_manifest.sha256,32)) goto done;
    /* esp_ota_end verifies the actual ESP image and chip; hash alone does not suffice. */
    esp_err_t e=esp_ota_end(handle);opened=false;if(e!=ESP_OK) goto done;
    esp_app_desc_t desc;
    if(esp_ota_get_partition_description(slot,&desc)!=ESP_OK||strcmp(desc.version,s_manifest.version)||
       strcmp(desc.project_name,esp_app_get_description()->project_name)) goto done;
    ok=esp_ota_set_boot_partition(slot)==ESP_OK;
done:
    if(opened) esp_ota_abort(handle);
    mbedtls_sha256_free(&hash);esp_http_client_cleanup(c);return ok;
}
static void worker(void *arg) {
    (void)arg;
    for(;;) {
        uint32_t bits=0;xTaskNotifyWait(0,UINT32_MAX,&bits,portMAX_DELAY);
        monitor_config_t config;monitor_config_snapshot(&config);
        if(!monitor_config_valid(&config)) {message("Configure network first",false);continue;}
        bool apply=(bits&2)!=0;
        if(apply&&!s_view.available) continue;
        server_monitor_pause(true);message(apply?"Verifying firmware update":"Checking manifest",true);
        uint64_t deadline=millis()+15000;
        while(!server_monitor_quiescent()&&millis()<deadline) vTaskDelay(pdMS_TO_TICKS(100));
        bool ok=false;
        if(server_monitor_quiescent()) {
            if(apply) {monitor_emit(EV_OTA_STARTED,NULL,0);ok=install(&config);}
            else ok=check(&config);
        }
        memset(&config,0,sizeof(config));
        if(!ok) {message("OTA failed; current app kept",false);monitor_emit(EV_OTA_FAILED,NULL,0);}
        else if(apply) {message("Verified; restarting",false);vTaskDelay(pdMS_TO_TICKS(1000));esp_restart();}
        server_monitor_pause(false);
    }
}
static void command(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;(void)id;int cmd=((monitor_command_data_t *)data)->command;
    if(cmd==CMD_OTA_CHECK||cmd==CMD_OTA_INSTALL) xTaskNotify(s_task,cmd==CMD_OTA_CHECK?1:2,eSetBits);
}
esp_err_t monitor_ota_start(void) {
    s_lock=xSemaphoreCreateMutex();if(!s_lock) return ESP_ERR_NO_MEM;
    strcpy(s_view.message,"OK: check HTTPS manifest");
    if(xTaskCreate(worker,"ota_worker",6144,NULL,2,&s_task)!=pdPASS) return ESP_ERR_NO_MEM;
    return monitor_subscribe(EV_COMMAND,command,NULL);
}
void monitor_ota_self_check(bool healthy) {
    if(!healthy||atomic_load(&s_checked)) return;
    const esp_partition_t *running=esp_ota_get_running_partition();esp_ota_img_states_t state;
    if(esp_ota_get_state_partition(running,&state)==ESP_OK&&state==ESP_OTA_IMG_PENDING_VERIFY) {
        if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK) return;
        monitor_emit(EV_OTA_SUCCESS,NULL,0);
    }
    atomic_store(&s_checked,true);
}
