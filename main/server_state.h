#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERVER_JSON_MAX 4096
typedef enum { SERVER_OFFLINE, SERVER_ONLINE, SERVER_STALE } server_status_t;
typedef enum { LIVE_WSS, HTTPS_POLLING, RECONNECTING, CONNECTION_OFFLINE } connection_mode_t;
typedef struct {
    bool online, has_data, temperature_valid;
    server_status_t status;
    uint64_t last_update; /* monotonic milliseconds, never server wall time */
    uint64_t timestamp, uptime, seq;
    connection_mode_t connection_mode;
    uint32_t reconnect_count;
    uint64_t wss_last_rx;
    char alert_message[96];
    int alert_level;
    uint32_t latency_ms;
    char hostname[64];
    float cpu_usage, load1, load5, load15, cpu_temperature;
    uint64_t memory_total, memory_used, disk_total, disk_used;
    float memory_percent, disk_percent;
    double network_rx_bps, network_tx_bps;
    uint64_t network_rx_bytes, network_tx_bytes;
    bool service_nginx, service_mariadb, service_php_fpm;
    char error[32];
} server_state_t;

/* Invalid payloads leave output unchanged. Temperature null is valid. */
bool server_state_parse(const char *json, size_t length, server_state_t *output);
void server_state_age(server_state_t *state, uint64_t now_ms);
unsigned server_retry_seconds(unsigned failures);
unsigned wifi_retry_seconds(unsigned failures);
bool server_url_valid(const char *url);
const char *server_status_name(server_status_t status);

/* Commit a newer snapshot atomically; preserve diagnostics/alerts across transports. */
bool server_state_commit(server_state_t *state, const server_state_t *fresh, connection_mode_t mode, uint64_t now);
bool server_should_poll(unsigned wss_failures, bool saver);
