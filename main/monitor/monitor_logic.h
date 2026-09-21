#pragma once
#include "server_state.h"
typedef struct { uint8_t cpu[60], ram[60], head, count; uint64_t next; } monitor_history_t;
void monitor_history_add(monitor_history_t *h, const server_state_t *s, uint64_t now);
unsigned monitor_alert_edges(uint8_t *active, const server_state_t *s);
uint32_t monitor_backoff_ms(unsigned failures, uint32_t random);
typedef struct { uint64_t last[32]; uint32_t seen; } monitor_cooldown_t;
bool monitor_cooldown(monitor_cooldown_t *c, unsigned event, uint64_t now, bool critical);
unsigned monitor_volume(int value);
void monitor_format_rate(char *out, size_t size, double bytes);
void monitor_format_uptime(char *out, size_t size, uint64_t seconds);
bool monitor_wss_url_valid(const char *url);
typedef struct { char version[32], url[256]; uint8_t sha256[32]; uint32_t size; } monitor_manifest_t;
bool monitor_version_valid(const char *version);
int monitor_version_compare(const char *a, const char *b);
bool monitor_manifest_parse(const char *json, size_t size, monitor_manifest_t *out);
bool monitor_alert_parse(const char *json, size_t size, int *level, float *value, char *source, char *message, uint64_t *seq, uint64_t *timestamp);
typedef struct { char data[SERVER_JSON_MAX+1]; size_t used, frame_offset; bool active; } monitor_ws_buffer_t;
/* Return 1 complete, 0 pending, -1 rejected. Control frames are handled by IDF. */
int monitor_ws_chunk(monitor_ws_buffer_t *b, int opcode, bool fin, size_t frame_size, size_t offset, const char *data, size_t n);

bool monitor_audio_preempts(unsigned incoming, unsigned current);

#define MONITOR_TREND_WIDTH 192
#define MONITOR_TREND_HEIGHT 80
#define MONITOR_TREND_BYTES (MONITOR_TREND_WIDTH*MONITOR_TREND_HEIGHT/4)
/* Four-color I2 bitmap; no allocation or LVGL drawing-task fanout. */
void monitor_history_raster(const monitor_history_t *history,uint8_t *pixels,size_t size);
