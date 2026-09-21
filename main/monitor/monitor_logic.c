#include "monitor_logic.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
void monitor_history_add(monitor_history_t *h, const server_state_t *s, uint64_t now) {
    if (!s->has_data || !s->online || now < h->next) return;
    h->cpu[h->head] = (uint8_t)s->cpu_usage; h->ram[h->head] = (uint8_t)s->memory_percent;
    h->head = (h->head+1)%60; if (h->count < 60) h->count++; h->next = now+2000;
}
unsigned monitor_alert_edges(uint8_t *active, const server_state_t *s) {
    uint8_t next = (s->cpu_usage>=90 ? 1:0) | (s->memory_percent>=90 ? 2:0) | (s->disk_percent>=90 ? 4:0);
    unsigned edges = next & ~*active; *active=next; return edges;
}
uint32_t monitor_backoff_ms(unsigned n, uint32_t random) {
    const unsigned delay[] = {1000,2000,4000,8000,16000,30000};
    unsigned base=delay[n>5?5:n]; return base - base/10 + random%(base/10+1);
}
bool monitor_cooldown(monitor_cooldown_t *c, unsigned event, uint64_t now, bool critical) {
    if (event>=32) return false;
    if ((c->seen & (1u<<event)) && now-c->last[event] < (critical?60000u:30000u)) return false;
    c->seen |= 1u<<event; c->last[event]=now; return true;
}
unsigned monitor_volume(int v) { return v<0?0:v>100?100:(unsigned)v; }
void monitor_format_rate(char *out, size_t size, double bytes) {
    const char *unit="B/s";
    if (bytes>=1e9) { bytes/=1e9; unit="GB/s"; }
    else if (bytes>=1e6) { bytes/=1e6; unit="MB/s"; }
    else if (bytes>=1e3) { bytes/=1e3; unit="KB/s"; }
    snprintf(out,size,"%.1f %s",bytes,unit);
}
void monitor_format_uptime(char *out, size_t size, uint64_t seconds) {
    snprintf(out,size,"%llud %02lluh %02llum",(unsigned long long)(seconds/86400),(unsigned long long)(seconds/3600%24),(unsigned long long)(seconds/60%60));
}
bool monitor_wss_url_valid(const char *url) {
    if (!url || strncmp(url,"wss://",6) || strlen(url)>253) return false;
    char https[256]; snprintf(https,sizeof(https),"https://%s",url+6); return server_url_valid(https);
}
bool monitor_version_valid(const char *v) {
    if (!v || strlen(v)>31) return false;
    unsigned dots=0; const char *p=v;
    for (unsigned part=0;part<3;part++) {
        if (!isdigit((unsigned char)*p)) return false;
        unsigned n=0;
        do { n=n*10+(*p++-'0'); if(n>65535) return false; } while (isdigit((unsigned char)*p));
        if (dots==2) break;
        if (*p++!='.') return false;
        dots++;
    }
    if (dots!=2) return false;
    if (!*p) return true;
    if (strncmp(p,"-beta.",6)) return false;
    p+=6; if(!isdigit((unsigned char)*p)) return false;
    unsigned n=0;
    while(isdigit((unsigned char)*p)) { n=n*10+(*p++-'0'); if(n>65535) return false; }
    return !*p;
}
int monitor_version_compare(const char *a,const char *b) {
    if(!monitor_version_valid(a)||!monitor_version_valid(b)) return 0;
    for(int i=0;i<3;i++) {
        char *ae,*be; unsigned long av=strtoul(a,&ae,10),bv=strtoul(b,&be,10);
        if(av!=bv) return av>bv?1:-1;
        a=ae; b=be; if(i<2) {a++;b++;}
    }
    if(!*a || !*b) return !*a && !*b?0:!*a?1:-1;
    unsigned long av=strtoul(a+6,NULL,10),bv=strtoul(b+6,NULL,10); return av==bv?0:av>bv?1:-1;
}
static bool text(const cJSON *r,const char *key,char *out,size_t size) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(r,key);
    if(!cJSON_IsString(v)||!v->valuestring||!v->valuestring[0]||strlen(v->valuestring)>=size) return false;
    for(const unsigned char *p=(const unsigned char *)v->valuestring;*p;p++) if(*p<32||*p>=127) return false;
    strcpy(out,v->valuestring); return true;
}
/* Reject nesting before invoking cJSON; manifests and alerts are flat objects. */
static cJSON *flat(const char *json,size_t n) {
    if(!json||!n||n>1024) return NULL;
    bool quoted=false,escape=false; unsigned depth=0;
    for(size_t i=0;i<n;i++) {
        char c=json[i]; if(!c) return NULL;
        if(quoted) {if(escape) escape=false; else if(c=='\\') escape=true; else if(c=='"') quoted=false;}
        else if(c=='"') quoted=true;
        else if(c=='[') return NULL;
        else if(c=='{') {if(++depth>1) return NULL;}
        else if(c=='}') {if(!depth) return NULL; depth--;}
    }
    if(quoted||depth) return NULL;
    const char *end=NULL; cJSON *r=cJSON_ParseWithLengthOpts(json,n,&end,false);
    if(!r) return NULL;
    while(end<json+n && isspace((unsigned char)*end)) end++;
    if(!cJSON_IsObject(r)||end!=json+n) {cJSON_Delete(r); return NULL;}
    for(cJSON *a=r->child;a;a=a->next) for(cJSON *b=a->next;b;b=b->next)
        if(!strcmp(a->string,b->string)) {cJSON_Delete(r); return NULL;}
    return r;
}
bool monitor_manifest_parse(const char *json,size_t size,monitor_manifest_t *out) {
    cJSON *r=flat(json,size); if(!r||!out) {cJSON_Delete(r);return false;}
    monitor_manifest_t m={0}; char hash[65]; cJSON *n=cJSON_GetObjectItemCaseSensitive(r,"size");
    bool ok=text(r,"version",m.version,sizeof(m.version))&&monitor_version_valid(m.version)&&
        text(r,"url",m.url,sizeof(m.url))&&server_url_valid(m.url)&&text(r,"sha256",hash,sizeof(hash))&&strlen(hash)==64&&
        cJSON_IsNumber(n)&&isfinite(n->valuedouble)&&n->valuedouble>=1024&&n->valuedouble<=0x300000&&floor(n->valuedouble)==n->valuedouble;
    if(ok) {
        for(unsigned i=0;i<32;i++) {
            if(!isxdigit((unsigned char)hash[2*i])||!isxdigit((unsigned char)hash[2*i+1])) {ok=false;break;}
            char pair[]={hash[2*i],hash[2*i+1],0}; m.sha256[i]=(uint8_t)strtoul(pair,NULL,16);
        }
        m.size=(uint32_t)n->valuedouble;
    }
    if(ok) *out=m;
    cJSON_Delete(r); return ok;
}
bool monitor_alert_parse(const char *json,size_t size,int *level,float *value,char *source,char *message,uint64_t *seq,uint64_t *timestamp) {
    cJSON *r=flat(json,size); if(!r) return false;
    char type[24],sev[16],src[24],msg[96];
    cJSON *v=cJSON_GetObjectItemCaseSensitive(r,"v"),*sq=cJSON_GetObjectItemCaseSensitive(r,"seq"),
          *ts=cJSON_GetObjectItemCaseSensitive(r,"timestamp"),*val=cJSON_GetObjectItemCaseSensitive(r,"value");
    bool ok=cJSON_IsNumber(v)&&v->valuedouble==1&&text(r,"type",type,sizeof(type))&&!strcmp(type,"alert")&&
        text(r,"level",sev,sizeof(sev))&&(!strcmp(sev,"info")||!strcmp(sev,"warning")||!strcmp(sev,"critical"))&&
        cJSON_IsNumber(sq)&&sq->valuedouble>=0&&sq->valuedouble<=9007199254740991.0&&floor(sq->valuedouble)==sq->valuedouble&&
        cJSON_IsNumber(ts)&&ts->valuedouble>0&&ts->valuedouble<=9007199254740991.0&&floor(ts->valuedouble)==ts->valuedouble&&
        cJSON_IsNumber(val)&&isfinite(val->valuedouble)&&fabs(val->valuedouble)<=1e15&&
        text(r,"source",src,sizeof(src))&&text(r,"message",msg,sizeof(msg));
    if(ok) { *level=!strcmp(sev,"critical")?2:!strcmp(sev,"warning")?1:0; *value=(float)val->valuedouble;
        strcpy(source,src);strcpy(message,msg);*seq=(uint64_t)sq->valuedouble;*timestamp=(uint64_t)ts->valuedouble; }
    cJSON_Delete(r);return ok;
}
int monitor_ws_chunk(monitor_ws_buffer_t *b,int opcode,bool fin,size_t frame_size,size_t offset,const char *data,size_t n) {
    if(opcode==1 && offset==0) { if(b->active) goto bad; b->used=0;b->frame_offset=0;b->active=true; }
    if((opcode!=0&&opcode!=1)||!b->active||offset!=b->frame_offset||offset>frame_size||n>frame_size-offset||frame_size>SERVER_JSON_MAX-b->used+offset||n>SERVER_JSON_MAX-b->used) goto bad;
    if(n) memcpy(b->data+b->used,data,n);
    b->used+=n;b->frame_offset+=n;
    if(offset+n==frame_size) {
        b->frame_offset=0;
        if(fin) {b->data[b->used]=0;b->active=false;return 1;}
    }
    return 0;
bad: b->used=0;b->frame_offset=0;b->active=false;return -1;
}

bool monitor_audio_preempts(unsigned incoming,unsigned current) {return incoming<3 && current<3 && incoming>current;}

static void trend_pixel(uint8_t *pixels,int x,int y,unsigned color) {
    if(x<0||x>=MONITOR_TREND_WIDTH||y<0||y>=MONITOR_TREND_HEIGHT) return;
    unsigned index=(unsigned)y*(MONITOR_TREND_WIDTH/4)+(unsigned)x/4,shift=6-2*((unsigned)x%4);
    pixels[index]=(uint8_t)((pixels[index]&~(3u<<shift))|(color<<shift));
}
static void trend_line(uint8_t *pixels,int x0,int y0,int x1,int y1,unsigned color) {
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,error=dx+dy;
    for(;;) {
        trend_pixel(pixels,x0,y0,color);if(x0==x1&&y0==y1) break;
        int twice=2*error;if(twice>=dy) {error+=dy;x0+=sx;}if(twice<=dx) {error+=dx;y0+=sy;}
    }
}
void monitor_history_raster(const monitor_history_t *h,uint8_t *pixels,size_t size) {
    if(!h||!pixels||size<MONITOR_TREND_BYTES) return;
    memset(pixels,0,MONITOR_TREND_BYTES);
    for(int y=0;y<MONITOR_TREND_HEIGHT;y+=39) for(int x=0;x<MONITOR_TREND_WIDTH;x++) trend_pixel(pixels,x,y,3);
    unsigned count=h->count>60?60:h->count;
    for(unsigned series=0;series<2;series++) {
        int lastx=0,lasty=0;
        for(unsigned i=0;i<count;i++) {
            unsigned at=(h->head+60-count+i)%60,value=series?h->ram[at]:h->cpu[at];if(value>100)value=100;
            int x=(int)((60-count+i)*(MONITOR_TREND_WIDTH-1)/59),y=(int)((100-value)*(MONITOR_TREND_HEIGHT-1)/100);
            if(i) trend_line(pixels,lastx,lasty,x,y,series+1);else trend_pixel(pixels,x,y,series+1);
            lastx=x;lasty=y;
        }
    }
}
