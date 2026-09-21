#include "server_monitor.h"
#include "monitor_events.h"
#include "monitor_logic.h"
#include "monitor_power.h"
#include "esp_websocket_client.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include <stdatomic.h>
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
/* Only one TLS transport runs at a time. Stop/join WSS before reusing RX for HTTP. */
static union { char http[SERVER_JSON_MAX+1]; monitor_ws_buffer_t ws; } s_rx_workspace;
#define s_body s_rx_workspace.http
#define s_rx s_rx_workspace.ws
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

static QueueHandle_t s_messages;

typedef struct { bool status; union {server_state_t state;monitor_alert_t alert;} value; } ws_message_t;
static ws_message_t s_packet, s_pending;
static atomic_bool s_ws_connected, s_ws_failed, s_paused, s_quiescent;
static atomic_uint s_progress;
static bool s_first_sync, s_server_was_online, s_ever_online;
static uint8_t s_alert_active;
static uint64_t s_alert_seq, s_alert_timestamp;
static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if(id==WEBSOCKET_EVENT_CONNECTED) {atomic_store(&s_ws_connected,true);monitor_emit(EV_WSS_CONNECTED,NULL,0);}
    else if(id==WEBSOCKET_EVENT_DISCONNECTED||id==WEBSOCKET_EVENT_CLOSED||id==WEBSOCKET_EVENT_ERROR) {
        if(atomic_exchange(&s_ws_connected,false)) monitor_emit(EV_WSS_DISCONNECTED,NULL,0);
        atomic_store(&s_ws_failed,true);
    } else if(id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *e=data;
        if(e->op_code>=8) return;
        if(e->data_len<0||e->payload_len<0||e->payload_offset<0) {atomic_store(&s_ws_failed,true);return;}
        int result=monitor_ws_chunk(&s_rx,e->op_code,e->fin,e->payload_len,e->payload_offset,e->data_ptr,e->data_len);
        if(result<0) {atomic_store(&s_ws_failed,true);return;}
        if(result==1) {
            s_packet.status=server_state_parse(s_rx.data,s_rx.used,&s_packet.value.state);
            if(!s_packet.status) {
                monitor_alert_t *a=&s_packet.value.alert;
                if(!monitor_alert_parse(s_rx.data,s_rx.used,&a->level,&a->value,a->source,a->message,&a->seq,&a->timestamp)) return;
            }
            /* Bounded queue. Losing a protocol message forces resync instead of silently losing alerts. */
            if(xQueueSend(s_messages,&s_packet,0)!=pdTRUE) atomic_store(&s_ws_failed,true);
        }
    }
}
static void close_ws(esp_websocket_client_handle_t *ws) {
    if(*ws) {esp_websocket_client_stop(*ws);esp_websocket_client_destroy(*ws);*ws=NULL;}
    if(atomic_exchange(&s_ws_connected,false)) monitor_emit(EV_WSS_DISCONNECTED,NULL,0);
    memset(&s_rx,0,sizeof(s_rx));xQueueReset(s_messages);
}
static void set_mode(connection_mode_t mode) {
    xSemaphoreTake(s_lock,portMAX_DELAY);bool changed=s_state.connection_mode!=mode;s_state.connection_mode=mode;xSemaphoreGive(s_lock);
    if(changed) {monitor_emit(EV_STATE_CHANGED,NULL,0);if(mode==HTTPS_POLLING) monitor_emit(EV_HTTPS_FALLBACK_STARTED,NULL,0);}
}
static bool accept_status(server_state_t *fresh,connection_mode_t mode) {
    time_t wall=time(NULL);
    if((double)fresh->timestamp>(double)wall+30 || (double)wall-(double)fresh->timestamp>15) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    /* Timestamp permits a restarted agent with a reset sequence, but rejects cached/replayed snapshots. */
    bool accept=server_state_commit(&s_state,fresh,mode,millis());
    xSemaphoreGive(s_lock);
    if(!accept) return false;
    if(!s_first_sync) {s_first_sync=true;monitor_emit(EV_SERVER_SYNC_FIRST_SUCCESS,NULL,0);}
    if(!s_server_was_online) {monitor_emit(s_ever_online?EV_SERVER_RECOVERED:EV_SERVER_ONLINE,NULL,0);s_server_was_online=true;s_ever_online=true;}
    unsigned edges=monitor_alert_edges(&s_alert_active,fresh);
    const char *sources[]={"cpu","ram","disk"};const char *messages[]={"CPU HIGH","RAM HIGH","DISK HIGH"};
    float values[]={fresh->cpu_usage,fresh->memory_percent,fresh->disk_percent};
    for(unsigned i=0;i<3;i++) if(edges&(1u<<i)) {
        monitor_alert_t alert={.level=2,.value=values[i]};strcpy(alert.source,sources[i]);strcpy(alert.message,messages[i]);
        monitor_emit(EV_SERVER_ALERT,&alert,sizeof(alert));
        xSemaphoreTake(s_lock,portMAX_DELAY);strcpy(s_state.alert_message,alert.message);s_state.alert_level=2;xSemaphoreGive(s_lock);
    }
    monitor_emit(EV_STATE_CHANGED,NULL,0);return true;
}
static bool accept_ws(const ws_message_t *packet) {
    if(packet->status) {server_state_t fresh=packet->value.state;return accept_status(&fresh,LIVE_WSS);}
    monitor_alert_t alert=packet->value.alert;
    time_t wall=time(NULL);
    if((double)alert.timestamp>(double)wall+30||(double)wall-(double)alert.timestamp>15||
       alert.timestamp<s_alert_timestamp||(alert.timestamp==s_alert_timestamp&&alert.seq<=s_alert_seq)) return false;
    s_alert_seq=alert.seq;s_alert_timestamp=alert.timestamp;
    xSemaphoreTake(s_lock,portMAX_DELAY);strcpy(s_state.alert_message,alert.message);s_state.alert_level=alert.level;s_state.wss_last_rx=millis();xSemaphoreGive(s_lock);
    monitor_emit(EV_SERVER_ALERT,&alert,sizeof(alert));return true;
}
static void command_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;(void)id;
    if(((monitor_command_data_t *)data)->command==CMD_REFRESH) server_monitor_refresh();
}
void server_monitor_pause(bool pause) {atomic_store(&s_paused,pause);}
bool server_monitor_progress(void) {return atomic_load(&s_progress)>10;}
bool server_monitor_quiescent(void) {return atomic_load(&s_quiescent);}
static void server_monitor_task(void *arg) {
    (void)arg;
    esp_websocket_client_handle_t ws=NULL;
    uint64_t retry=0,poll=0,started=0,last_valid=0;
    unsigned failures=0;uint32_t revision=UINT32_MAX;
    static monitor_config_t config;
    for(;;) {
        bool manual=ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100))>0;
        atomic_fetch_add(&s_progress,1);
        uint64_t now=millis();uint32_t current=revision;
        bool ready=monitor_wifi_config(&config,&current);
        if(current!=revision) {close_ws(&ws);revision=current;failures=0;retry=poll=0;last_valid=0;
            xSemaphoreTake(s_lock,portMAX_DELAY);memset(&s_state,0,sizeof(s_state));s_state.connection_mode=RECONNECTING;xSemaphoreGive(s_lock);
            s_alert_seq=s_alert_timestamp=0;s_alert_active=0;s_server_was_online=false;
        }
        if(!ready||time(NULL)<1704067200||atomic_load(&s_paused)) {
            close_ws(&ws);set_mode(CONNECTION_OFFLINE);
            atomic_store(&s_quiescent,true);
            xSemaphoreTake(s_lock,portMAX_DELAY);server_state_age(&s_state,millis());bool offline=s_state.status==SERVER_OFFLINE;xSemaphoreGive(s_lock);
            if(offline&&s_server_was_online) {s_server_was_online=false;monitor_emit(EV_SERVER_OFFLINE,NULL,0);}
            continue;
        }
        atomic_store(&s_quiescent,false);
        bool saver=monitor_power_ambient()&&config.power_mode==2;
        if(saver) close_ws(&ws);
        if(ws && (atomic_load(&s_ws_failed)||now-(last_valid?last_valid:started)>30000)) {
            close_ws(&ws);retry=now+monitor_backoff_ms(failures,esp_random());if(failures<10) failures++;
            xSemaphoreTake(s_lock,portMAX_DELAY);s_state.reconnect_count++;snprintf(s_state.error,sizeof(s_state.error),"WSS retry / timeout");xSemaphoreGive(s_lock);
            set_mode(RECONNECTING);
        }
        if(!ws&&!saver&&now>=retry && !(server_should_poll(failures,saver)&&(now>=poll||manual))) {
            char header[160];snprintf(header,sizeof(header),"Authorization: Bearer %s\r\n",config.token);
            esp_websocket_client_config_t wc={.uri=config.ws_url,.headers=header,.transport=WEBSOCKET_TRANSPORT_OVER_SSL,
                .crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_reconnect=true,.task_stack=6144,.task_prio=3,
                .buffer_size=1024,.network_timeout_ms=3000,.ping_interval_sec=20,.pingpong_timeout_sec=10};
            atomic_store(&s_ws_failed,false);ws=esp_websocket_client_init(&wc);memset(header,0,sizeof(header));
            started=now;last_valid=0;monitor_emit(EV_WSS_CONNECTING,NULL,0);
            if(!ws||esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,websocket_event,NULL)!=ESP_OK||esp_websocket_client_start(ws)!=ESP_OK) {
                close_ws(&ws);retry=now+monitor_backoff_ms(failures,esp_random());if(failures<10) failures++;
            }
        }
        while(xQueueReceive(s_messages,&s_pending,0)==pdTRUE) {
            if(accept_ws(&s_pending)) {last_valid=millis();failures=0;set_mode(LIVE_WSS);}
        }
        if(!ws && server_should_poll(failures,saver) && (now>=poll||manual)) {
            response_meta_t meta={0};server_state_t fresh;char error[32]={0};char auth[144];
            esp_http_client_config_t hc={.url=config.url,.method=HTTP_METHOD_GET,.timeout_ms=3000,.crt_bundle_attach=esp_crt_bundle_attach,
                .transport_type=HTTP_TRANSPORT_OVER_SSL,.disable_auto_redirect=true,.max_authorization_retries=-1,
                .buffer_size=1024,.buffer_size_tx=1024,.event_handler=on_header,.user_data=&meta};
            esp_http_client_handle_t client=esp_http_client_init(&hc);bool success=false;
            if(client) {
                snprintf(auth,sizeof(auth),"Bearer %s",config.token);
                if(esp_http_client_set_header(client,"Authorization",auth)==ESP_OK &&
                   esp_http_client_set_header(client,"Accept","application/json")==ESP_OK) success=fetch(client,&meta,&fresh,error,sizeof(error));
                memset(auth,0,sizeof(auth));esp_http_client_cleanup(client);
            }
            if(success) {set_mode(HTTPS_POLLING);accept_status(&fresh,HTTPS_POLLING);}
            else {monitor_emit(EV_SERVER_SYNC_FAILED,NULL,0);xSemaphoreTake(s_lock,portMAX_DELAY);snprintf(s_state.error,sizeof(s_state.error),"%s",client?error:"HTTP memory unavailable");xSemaphoreGive(s_lock);}
            poll=millis()+(saver?60000:10000);
        }
        xSemaphoreTake(s_lock,portMAX_DELAY);server_state_age(&s_state,millis());bool offline=s_state.status==SERVER_OFFLINE;xSemaphoreGive(s_lock);
        if(offline&&s_server_was_online) {s_server_was_online=false;monitor_emit(EV_SERVER_OFFLINE,NULL,0);}
    }
}
esp_err_t server_monitor_start(void) {
    s_lock=xSemaphoreCreateMutex();s_deadline_lock=xSemaphoreCreateMutex();s_messages=xQueueCreate(6,sizeof(ws_message_t));
    if(!s_lock||!s_deadline_lock||!s_messages) return ESP_ERR_NO_MEM;
    s_state.connection_mode=CONNECTION_OFFLINE;
    if(xTaskCreate(deadline_task,"http_deadline",2048,NULL,4,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    if(monitor_subscribe(EV_COMMAND,command_event,NULL)!=ESP_OK) return ESP_FAIL;
    if(xTaskCreate(server_monitor_task,"server_monitor",8192,NULL,3,&s_task)!=pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
