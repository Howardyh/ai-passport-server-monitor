#include "monitor_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern int16_t monitor_ulaw_decode(uint8_t value);
int main(void) {
    monitor_history_t h={0};server_state_t s={.has_data=true,.online=true,.cpu_usage=91,.memory_percent=90,.disk_percent=1};
    for(unsigned i=0;i<70;i++) monitor_history_add(&h,&s,i*2000);
    assert(h.count==60&&h.head==10&&h.cpu[9]==91);monitor_history_add(&h,&s,138001);assert(h.head==10);
    s.online=false;monitor_history_add(&h,&s,150000);assert(h.head==10);
    for(unsigned i=0;i<60;i++) h.ram[i]=50;
    uint8_t pixels[MONITOR_TREND_BYTES+2];memset(pixels,0x55,sizeof(pixels));
    monitor_history_raster(&h,pixels+1,MONITOR_TREND_BYTES);
    assert(pixels[0]==0x55&&pixels[sizeof(pixels)-1]==0x55);
    unsigned cpu_pixels=0,ram_pixels=0;
    for(unsigned i=1;i<=MONITOR_TREND_BYTES;i++) for(unsigned shift=0;shift<8;shift+=2) {
        unsigned color=(pixels[i]>>shift)&3;cpu_pixels+=color==1;ram_pixels+=color==2;
    }
    assert(cpu_pixels&&ram_pixels);
    uint8_t active=0;assert(monitor_alert_edges(&active,&s)==3);assert(monitor_alert_edges(&active,&s)==0);
    s.cpu_usage=89;assert(monitor_alert_edges(&active,&s)==0);s.cpu_usage=95;assert(monitor_alert_edges(&active,&s)==1);
    monitor_cooldown_t c={0};assert(monitor_cooldown(&c,0,0,false));assert(!monitor_cooldown(&c,0,29999,false));assert(monitor_cooldown(&c,0,30000,false));
    assert(monitor_cooldown(&c,1,0,true));assert(!monitor_cooldown(&c,1,59999,true));assert(monitor_cooldown(&c,1,60000,true));assert(!monitor_cooldown(&c,32,0,false));
    assert(monitor_volume(-1)==0&&monitor_volume(101)==100&&monitor_volume(60)==60);
    unsigned backoff[]={1000,2000,4000,8000,16000,30000};
    for(unsigned i=0;i<20;i++) for(unsigned r=0;r<1000;r++) {unsigned b=backoff[i>5?5:i],n=monitor_backoff_ms(i,r);assert(n>=b-b/10&&n<=b);}
    char out[40];monitor_format_rate(out,sizeof(out),12400);assert(!strcmp(out,"12.4 KB/s"));monitor_format_rate(out,sizeof(out),1e9);assert(!strcmp(out,"1.0 GB/s"));
    monitor_format_uptime(out,sizeof(out),90061);assert(!strcmp(out,"1d 01h 01m"));
    assert(monitor_wss_url_valid("wss://status.example.com/ws"));assert(!monitor_wss_url_valid("ws://x/ws"));assert(!monitor_wss_url_valid("wss://x/ws?token=abc"));
    const char *bad[]={"1.2.","1.2","1.2.3-","1.2.3-beta.","1.2.3-beta.x","9999999999.0.0","v1.2.3","1.2.3 extra"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!monitor_version_valid(bad[i]));
    assert(monitor_version_valid("0.2.0-beta.1"));assert(monitor_version_compare("0.2.0-beta.10","0.2.0-beta.2")==1);
    assert(monitor_version_compare("0.2.0","0.2.0-beta.10")==1);assert(monitor_version_compare("0.1.0","0.2.0")==-1);
    const char *manifest="{\"version\":\"0.2.0-beta.2\",\"url\":\"https://status.example.com/firmware/app.bin\",\"size\":1500000,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
    monitor_manifest_t m;assert(monitor_manifest_parse(manifest,strlen(manifest),&m)&&m.size==1500000&&m.sha256[31]==0xaa);
    assert(!monitor_manifest_parse("{}",2,&m));assert(!monitor_manifest_parse("{\"x\":{}}",8,&m));
    const char *alert="{\"v\":1,\"type\":\"alert\",\"seq\":3,\"timestamp\":1790000001,\"level\":\"critical\",\"source\":\"cpu\",\"value\":96.4,\"message\":\"CPU HIGH\"}";
    int level;float value;char source[24],message[96];uint64_t seq,ts;
    assert(monitor_alert_parse(alert,strlen(alert),&level,&value,source,message,&seq,&ts));assert(level==2&&seq==3&&!strcmp(message,"CPU HIGH"));
    assert(!monitor_alert_parse("{}",2,&level,&value,source,message,&seq,&ts));
    monitor_ws_buffer_t ws={0};
    assert(monitor_ws_chunk(&ws,1,true,6,0,"abc",3)==0);assert(monitor_ws_chunk(&ws,1,true,6,3,"def",3)==1);assert(!strcmp(ws.data,"abcdef"));
    assert(monitor_ws_chunk(&ws,1,false,3,0,"abc",3)==0);assert(monitor_ws_chunk(&ws,0,true,3,0,"def",3)==1);assert(!strcmp(ws.data,"abcdef"));
    assert(monitor_ws_chunk(&ws,0,true,1,0,"x",1)==-1);assert(monitor_ws_chunk(&ws,1,true,4097,0,"x",1)==-1);
    assert(monitor_ws_chunk(&ws,1,true,4,0,"ab",2)==0);assert(monitor_ws_chunk(&ws,1,true,4,3,"d",1)==-1);
    assert(monitor_ulaw_decode(0xff)==0&&monitor_ulaw_decode(0x7f)==0&&monitor_ulaw_decode(0x80)==32124&&monitor_ulaw_decode(0x00)==-32124);
    s.last_update=0;s.has_data=true;server_state_age(&s,16000);assert(s.status==SERVER_STALE);server_state_age(&s,61000);assert(s.status==SERVER_OFFLINE&&s.cpu_usage==95);
    assert(monitor_audio_preempts(2,0)&&monitor_audio_preempts(2,1)&&!monitor_audio_preempts(0,2)&&!monitor_audio_preempts(2,2));
    server_state_t shared={0},fresh={.has_data=true,.online=true,.timestamp=100,.seq=1,.cpu_usage=95};
    assert(!server_should_poll(2,false)&&server_should_poll(3,false)&&server_should_poll(0,true));
    assert(server_state_commit(&shared,&fresh,LIVE_WSS,1000)&&shared.connection_mode==LIVE_WSS);
    fresh.seq++;fresh.latency_ms=42;assert(server_state_commit(&shared,&fresh,HTTPS_POLLING,2000)&&shared.connection_mode==HTTPS_POLLING);
    assert(!server_state_commit(&shared,&fresh,LIVE_WSS,3000)&&shared.last_update==2000);
    fresh.seq++;assert(server_state_commit(&shared,&fresh,LIVE_WSS,4000)&&shared.connection_mode==LIVE_WSS&&shared.wss_last_rx==4000&&shared.latency_ms==42);
    fresh.timestamp=99;assert(!server_state_commit(&shared,&fresh,HTTPS_POLLING,5000)&&shared.cpu_usage==95);
    fresh.timestamp=101;fresh.seq=0;assert(server_state_commit(&shared,&fresh,LIVE_WSS,6000));
    puts("Monitor logic: PASS (history, alert edges, cooldown, bounds, jitter, formatting, version, manifest, alert, fragmentation, voice)");
}
