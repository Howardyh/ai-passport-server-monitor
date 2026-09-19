#include "server_state.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

static const cJSON *field(const cJSON *obj, const char *name) {
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, name) : NULL;
}
static bool number(const cJSON *obj, const char *key, double low, double high, double *out) {
    const cJSON *v = field(obj, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low || v->valuedouble > high) return false;
    *out = v->valuedouble;
    return true;
}
static bool integer(const cJSON *obj, const char *key, uint64_t *out) {
    double d;
    if (!number(obj, key, 0, 9007199254740991.0, &d) || floor(d) != d) return false;
    *out = (uint64_t)d;
    return true;
}
static bool real(const cJSON *obj, const char *key, double low, double high, float *out) {
    double d;
    if (!number(obj, key, low, high, &d)) return false;
    *out = (float)d;
    return true;
}
static bool boolean(const cJSON *obj, const char *key, bool *out) {
    const cJSON *v = field(obj, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}
static bool bounded_depth(const char *s, size_t n) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!c) return false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; depth--; }
    }
    return !quoted && !depth;
}
static bool unique_keys(const cJSON *obj) {
    for (const cJSON *a = obj->child; a; a = a->next) {
        if (cJSON_IsObject(obj)) {
            for (const cJSON *b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string)) return false;
        }
        if (a->child && !unique_keys(a)) return false;
    }
    return true;
}
bool server_state_parse(const char *json, size_t length, server_state_t *output) {
    if (!json || !output || !length || length > SERVER_JSON_MAX || !bounded_depth(json, length)) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;
    while (end < json + length && isspace((unsigned char)*end)) end++;
    server_state_t s = {0};
    double version;
    const cJSON *host = field(root, "hostname");
    const cJSON *cpu = field(root, "cpu"), *mem = field(root, "memory");
    const cJSON *disk = field(root, "disk"), *net = field(root, "network"), *services = field(root, "services");
    bool ok = end == json + length && cJSON_IsObject(root) && unique_keys(root) &&
        number(root, "version", 1, 1, &version) && integer(root, "timestamp", &s.timestamp) && s.timestamp > 0 &&
        integer(root, "uptime", &s.uptime) && cJSON_IsString(host) && host->valuestring &&
        strlen(host->valuestring) > 0 && strlen(host->valuestring) < sizeof(s.hostname) &&
        real(cpu, "usage", 0, 100, &s.cpu_usage) && real(cpu, "load1", 0, 100000, &s.load1) &&
        real(cpu, "load5", 0, 100000, &s.load5) && real(cpu, "load15", 0, 100000, &s.load15) &&
        integer(mem, "total", &s.memory_total) && s.memory_total > 0 && integer(mem, "used", &s.memory_used) &&
        s.memory_used <= s.memory_total && real(mem, "percent", 0, 100, &s.memory_percent) &&
        integer(disk, "total", &s.disk_total) && s.disk_total > 0 && integer(disk, "used", &s.disk_used) &&
        s.disk_used <= s.disk_total && real(disk, "percent", 0, 100, &s.disk_percent) &&
        integer(net, "rx_bytes", &s.network_rx_bytes) && integer(net, "tx_bytes", &s.network_tx_bytes) &&
        number(net, "rx_bps", 0, 1e15, &s.network_rx_bps) && number(net, "tx_bps", 0, 1e15, &s.network_tx_bps) &&
        boolean(services, "nginx", &s.service_nginx) && boolean(services, "mariadb", &s.service_mariadb) &&
        boolean(services, "php_fpm", &s.service_php_fpm);
    const cJSON *temp = field(cpu, "temperature");
    if (cJSON_IsNull(temp)) s.temperature_valid = false;
    else { s.temperature_valid = true; ok = ok && real(cpu, "temperature", -100, 250, &s.cpu_temperature); }
    if (ok) {
        /* The built-in font is ASCII. Reject control bytes; render UTF-8 host bytes safely. */
        size_t n = strlen(host->valuestring);
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)host->valuestring[i];
            if (c < 32 || c == 127) { ok = false; break; }
            s.hostname[i] = c < 127 ? (char)c : '?';
        }
    }
    if (ok) { s.has_data = true; s.online = true; s.status = SERVER_ONLINE; *output = s; }
    cJSON_Delete(root);
    return ok;
}
void server_state_age(server_state_t *s, uint64_t now_ms) {
    s->status = !s->has_data ? SERVER_OFFLINE :
        (now_ms >= s->last_update && now_ms - s->last_update > 15000 ? SERVER_STALE : SERVER_ONLINE);
    s->online = s->status == SERVER_ONLINE;
}
unsigned server_retry_seconds(unsigned failures) {
    const unsigned values[] = {5, 10, 20, 30, 60};
    return values[failures > 4 ? 4 : failures];
}
unsigned wifi_retry_seconds(unsigned failures) {
    const unsigned values[] = {2, 4, 8, 16, 30};
    return values[failures > 4 ? 4 : failures];
}
const char *server_status_name(server_status_t status) {
    return status == SERVER_ONLINE ? "ONLINE" : status == SERVER_STALE ? "STALE" : "OFFLINE";
}
bool server_url_valid(const char *url) {
    if (!url || strncmp(url, "https://", 8) || strlen(url) > 255) return false;
    const char *host = url + 8, *p = host;
    while (isalnum((unsigned char)*p) || *p == '.' || *p == '-') p++;
    if (p == host || *host == '.' || *host == '-' || p[-1] == '.' || p[-1] == '-') return false;
    if (*p == ':') {
        unsigned port = 0, count = 0;
        for (p++; isdigit((unsigned char)*p); p++) {
            if (++count > 5) return false;
            port = port * 10 + (unsigned)(*p - '0');
        }
        if (!count || !port || port > 65535) return false;
    }
    if (*p != '/') return false;
    for (; *p; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || strchr("?#@\\", *p)) return false;
    return true;
}
