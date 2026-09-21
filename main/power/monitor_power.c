#include "monitor_power.h"
#include "monitor_config.h"
#include "bsp_display.h"
#include <stdatomic.h>
static atomic_bool s_ambient;
static uint64_t s_last_key;
static int s_brightness=-1;
bool monitor_power_ambient(void) {return atomic_load(&s_ambient);}
bool monitor_power_key(uint64_t now) {
    bool asleep=atomic_exchange(&s_ambient,false);s_last_key=now;return asleep;
}
void monitor_power_tick(uint64_t now) {
    monitor_config_t c;monitor_config_snapshot(&c);
    bool ambient=c.screen_timeout&&now-s_last_key>c.screen_timeout*1000ull;
    atomic_store(&s_ambient,ambient);
    int brightness=ambient?(c.power_mode==1?10:0):c.brightness;
    if(brightness!=s_brightness) {bsp_display_backlight(brightness);s_brightness=brightness;}
}
