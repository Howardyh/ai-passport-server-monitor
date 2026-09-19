#include "server_state.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char valid[] =
"{\"version\":1,\"timestamp\":1790000000,\"hostname\":\"server\",\"uptime\":123456,"
"\"cpu\":{\"usage\":23.4,\"load1\":0.53,\"load5\":0.38,\"load15\":0.29,\"temperature\":null},"
"\"memory\":{\"total\":4096000000,\"used\":2100000000,\"percent\":51.3},"
"\"disk\":{\"total\":100000000000,\"used\":45000000000,\"percent\":45},"
"\"network\":{\"rx_bytes\":123456789,\"tx_bytes\":987654321,\"rx_bps\":13520,\"tx_bps\":3250},"
"\"services\":{\"nginx\":true,\"mariadb\":true,\"php_fpm\":false}}";

static void rejected(cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    server_state_t state, before;
    memset(&state, 0x5a, sizeof(state)); before = state;
    assert(!server_state_parse(json, strlen(json), &state));
    assert(!memcmp(&state, &before, sizeof(state)));
    cJSON_free(json); cJSON_Delete(root);
}
int main(void) {
    server_state_t state = {0};
    assert(server_state_parse(valid, strlen(valid), &state));
    assert(state.has_data && state.online && !state.temperature_valid && state.memory_total == 4096000000ULL);
    assert(!state.service_php_fpm && state.disk_total == 100000000000ULL);
    state.last_update = 1000;
    server_state_age(&state, 16000); assert(state.status == SERVER_ONLINE);
    server_state_age(&state, 16001); assert(state.status == SERVER_STALE && state.cpu_usage > 23);
    state.has_data = false; server_state_age(&state, 17000); assert(state.status == SERVER_OFFLINE);
    const char *top[] = {"version", "timestamp", "hostname", "uptime", "cpu", "memory", "disk", "network", "services"};
    for (size_t i = 0; i < sizeof(top)/sizeof(top[0]); i++) {
        cJSON *root = cJSON_Parse(valid); cJSON_DeleteItemFromObjectCaseSensitive(root, top[i]); rejected(root);
    }
    const char *groups[] = {"cpu", "memory", "disk", "network", "services"};
    for (size_t i = 0; i < 5; i++) {
        cJSON *template = cJSON_Parse(valid);
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(template, groups[i]);
        for (const cJSON *item = group->child; item; item = item->next) {
            cJSON *root = cJSON_Parse(valid), *obj = cJSON_GetObjectItemCaseSensitive(root, groups[i]);
            cJSON_ReplaceItemInObjectCaseSensitive(obj, item->string, cJSON_CreateString("invalid")); rejected(root);
            root = cJSON_Parse(valid); obj = cJSON_GetObjectItemCaseSensitive(root, groups[i]);
            cJSON_DeleteItemFromObjectCaseSensitive(obj, item->string); rejected(root);
        }
        cJSON_Delete(template);
    }
    const double bad[] = {-1, 100.1, 1e100};
    for (size_t i = 0; i < 3; i++) {
        cJSON *root = cJSON_Parse(valid);
        cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItem(root, "cpu"), "usage", cJSON_CreateNumber(bad[i])); rejected(root);
    }
    cJSON *root = cJSON_Parse(valid);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItem(root,"memory"), "used", cJSON_CreateNumber(1e12)); rejected(root);
    root = cJSON_Parse(valid); cJSON_AddNumberToObject(root, "version", 1); rejected(root);
    root = cJSON_Parse(valid);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItem(root,"cpu"), "temperature", cJSON_CreateNumber(51.2));
    char *json = cJSON_PrintUnformatted(root);
    assert(server_state_parse(json, strlen(json), &state) && state.temperature_valid);
    cJSON_free(json); cJSON_Delete(root);
    for (size_t i = 0; i < strlen(valid); i++) assert(!server_state_parse(valid, i, &state));
    assert(!server_state_parse("[[[[[[[[[0]]]]]]]]]", 19, &state));
    assert(!server_state_parse(valid, SERVER_JSON_MAX+1, &state));
    char trailing[sizeof(valid)+8]; snprintf(trailing, sizeof(trailing), "%s null", valid);
    assert(!server_state_parse(trailing, strlen(trailing), &state));
    assert(server_url_valid("https://status.example.com/api/v1/status"));
    assert(server_url_valid("https://status.example.com:8443/api/v1/status"));
    const char *bad_urls[] = {"http://x/a", "https:///a", "https://user:pass@x/a", "https://x/a\r\nX:1", "https://x/a?token=x", "https://x:0/a", "https://x:65536/a", "https://x/a#f", "https://x\\evil/a"};
    for (size_t i=0; i<sizeof(bad_urls)/sizeof(bad_urls[0]); i++) assert(!server_url_valid(bad_urls[i]));
    const unsigned wifi[] = {2,4,8,16,30}, api[] = {5,10,20,30,60};
    for (unsigned i=0; i<100; i++) {
        assert(wifi_retry_seconds(i)==wifi[i>4?4:i]); assert(server_retry_seconds(i)==api[i>4?4:i]);
    }
    puts("Server state: PASS (validation, retention, aging, depth, URL, backoff)");
    return 0;
}
