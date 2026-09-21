#include "captive_dns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include <string.h>
static atomic_bool s_enabled;
void captive_dns_enable(bool enabled) {atomic_store(&s_enabled,enabled);}
static void dns_task(void *arg) {
    (void)arg;int fd=-1;unsigned char packet[300];
    for(;;) {
        if(!atomic_load(&s_enabled)) {if(fd>=0) {close(fd);fd=-1;}vTaskDelay(pdMS_TO_TICKS(100));continue;}
        if(fd<0) {
            fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0) {vTaskDelay(pdMS_TO_TICKS(1000));continue;}
            struct timeval timeout={.tv_sec=0,.tv_usec=200000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(53),.sin_addr.s_addr=htonl(INADDR_ANY)};
            if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0) {close(fd);fd=-1;vTaskDelay(pdMS_TO_TICKS(1000));continue;}
        }
        struct sockaddr_in peer;socklen_t len=sizeof(peer);int n=recvfrom(fd,packet,256,0,(struct sockaddr *)&peer,&len);
        if(n<17||!atomic_load(&s_enabled)||packet[2]&0x80||packet[4]!=0||packet[5]!=1) continue;
        /* Only answer the AP subnet; never act as an upstream DNS service. */
        if((ntohl(peer.sin_addr.s_addr)&0xffffff00u)!=0xc0a80400u) continue;
        unsigned p=12;
        while(p<(unsigned)n&&packet[p]&&packet[p]<=63) {unsigned l=packet[p];if(p+1+l>=(unsigned)n) break;p+=1+l;}
        if(p+5!=(unsigned)n||packet[p]!=0||packet[p+1]!=0||packet[p+2]!=1||packet[p+3]!=0||packet[p+4]!=1) continue;
        packet[2]=0x81;packet[3]=0x80;packet[6]=0;packet[7]=1;memset(packet+8,0,4);
        const unsigned char answer[]={0xc0,0x0c,0,1,0,1,0,0,0,0,0,4,192,168,4,1};
        memcpy(packet+n,answer,sizeof(answer));sendto(fd,packet,n+sizeof(answer),0,(struct sockaddr *)&peer,len);
    }
}
esp_err_t captive_dns_start(void) {return xTaskCreate(dns_task,"captive_dns",2048,NULL,2,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;}
