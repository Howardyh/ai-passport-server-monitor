#include <stdint.h>
int16_t monitor_ulaw_decode(uint8_t value) {
    unsigned u=(unsigned)(uint8_t)~value;
    int pcm=(((u&15)<<3)+132)<<((u>>4)&7);
    return (int16_t)((u&128)?132-pcm:pcm-132);
}
