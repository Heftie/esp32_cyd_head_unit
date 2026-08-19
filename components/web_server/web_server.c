#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <mdns.h>
#include <cJSON.h>

#include "data_hub.h"
#include "logger.h"
#include "sd_storage.h"
#include "wifi_provision.h"

static const char *TAG = "web_server";

#define WIFI_BRINGUP_STACK   4096
#define WIFI_BRINGUP_PRIO    5
#define WIFI_BRINGUP_CORE    1 // Wi-Fi driver task defaults to core 0
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_CONNECTED_BIT   (1 << 0)

#define HISTORY_READ_CHUNK   512
#define HISTORY_LINE_MAX     96

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// 0 until an SNTP sync has landed. Once set, any boot-relative
// esp_timer_get_time()/logger timestamp_us converts to wall-clock epoch
// microseconds by adding this offset — including rows logged before Wi-Fi
// or SNTP came up, since esp_timer's clock started at the same boot.
static volatile int64_t s_boot_epoch_offset_us = 0;
static volatile bool s_time_synced = false;
static volatile web_server_time_source_t s_time_source = WEB_SERVER_TIME_UNSET;

// WiFi status for UI consumers (web_server_get_status()). Written once per
// bring-up outcome from wifi_bringup_task/ip_event_handler (WiFi task/event
// context), read from wherever a UI polls it — no lock, same convention as
// s_boot_epoch_offset_us/s_time_synced above.
static volatile web_server_wifi_mode_t s_wifi_mode = WEB_SERVER_WIFI_CONNECTING;
static char s_wifi_ssid[33] = {0};
static char s_wifi_ip[16] = {0};

static httpd_handle_t s_httpd;
static uint16_t s_http_port;
static EventGroupHandle_t s_wifi_event_group;

// --- Wi-Fi / SNTP / mDNS bring-up ------------------------------------------

static void sntp_sync_cb(struct timeval *tv)
{
    int64_t epoch_us = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
    s_boot_epoch_offset_us = epoch_us - esp_timer_get_time();
    s_time_synced = true;
    s_time_source = WEB_SERVER_TIME_NTP;
    ESP_LOGI(TAG, "SNTP synced");
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_sync_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_WEB_SERVER_MDNS_HOSTNAME);
    mdns_instance_name_set("CYD Telemetry");
    mdns_service_add(NULL, "_http", "_tcp", s_http_port, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", CONFIG_WEB_SERVER_MDNS_HOSTNAME);
}

static esp_err_t start_httpd(void);

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&evt->ip_info.ip));
    s_wifi_mode = WEB_SERVER_WIFI_STA;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Only the first connection needs to bring these up; reconnects after a
    // drop don't need a fresh mDNS/httpd instance.
    static bool started_once = false;
    if (!started_once) {
        started_once = true;
        start_sntp();
        start_mdns();
        if (start_httpd() != ESP_OK) {
            ESP_LOGE(TAG, "start_httpd failed");
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
    }
}

// Boot flow: load credentials from NVS (see wifi_provision.c) and try
// STA with a bounded wait. No stored credentials, or no connection within
// that window, falls back to a SoftAP + captive portal so the network can
// be (re)configured from a phone — no rebuild/reflash, no credentials in
// any file this repo tracks or even builds from.
static void wifi_bringup_task(void *arg)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    // AP netif is created lazily by wifi_provision_start_ap() only if we
    // actually fall back to it — creating it here unconditionally races
    // wifi_provision.c's own call and asserts (duplicate netif key).

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (wifi_provision_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "found stored credentials for \"%s\", connecting", ssid);
        strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);

        wifi_config_t sta_cfg = { 0 };
        strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            vTaskDelete(NULL);
            return; // SNTP/mDNS/httpd already kicked off from ip_event_handler
        }

        ESP_LOGW(TAG, "could not connect to \"%s\" within %d ms, falling back to setup AP",
                 ssid, WIFI_CONNECT_TIMEOUT_MS);
        esp_wifi_stop();
    } else {
        ESP_LOGW(TAG, "no stored WiFi credentials, starting setup AP");
    }

    char ap_ssid[33] = {0};
    char ap_ip[16] = {0};
    if (wifi_provision_start_ap(ap_ssid, sizeof(ap_ssid), ap_ip, sizeof(ap_ip)) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_provision_start_ap failed");
    } else {
        strncpy(s_wifi_ssid, ap_ssid, sizeof(s_wifi_ssid) - 1);
        strncpy(s_wifi_ip, ap_ip, sizeof(s_wifi_ip) - 1);
        s_wifi_mode = WEB_SERVER_WIFI_AP;
    }
    vTaskDelete(NULL);
}

// --- HTTP handlers -----------------------------------------------------

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[128];
    int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t api_data_handler(httpd_req_t *req)
{
    data_hub_channel_info_t channels[DATA_HUB_MAX_CHANNELS];
    size_t n = data_hub_list_channels(channels, DATA_HUB_MAX_CHANNELS);

    cJSON *root = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", channels[i].name);
        cJSON_AddNumberToObject(item, "value", channels[i].latest_value);
        cJSON_AddStringToObject(item, "unit", channels[i].unit);
        if (s_time_synced) {
            int64_t epoch_us = channels[i].latest_timestamp_us + s_boot_epoch_offset_us;
            cJSON_AddNumberToObject(item, "ts_epoch", (double)(epoch_us / 1000000));
        } else {
            cJSON_AddNullToObject(item, "ts_epoch");
        }
        cJSON_AddItemToArray(root, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

typedef struct {
    int64_t ts_epoch_us;
    char channel[DATA_HUB_NAME_LEN];
    float value;
    char unit[DATA_HUB_UNIT_LEN];
} log_row_t;

static bool parse_log_line(const char *line, log_row_t *out)
{
    long long ts_us = 0;
    char channel[DATA_HUB_NAME_LEN] = {0};
    float value = 0.0f;
    char unit[DATA_HUB_UNIT_LEN] = {0};

    // "timestamp_us,channel,value,unit" — the exact shape logger.c writes.
    int n = sscanf(line, "%lld,%15[^,],%f,%7s", &ts_us, channel, &value, unit);
    if (n != 4) {
        return false;
    }
    out->ts_epoch_us = (int64_t)ts_us + s_boot_epoch_offset_us;
    strncpy(out->channel, channel, sizeof(out->channel) - 1);
    out->channel[sizeof(out->channel) - 1] = '\0';
    out->value = value;
    strncpy(out->unit, unit, sizeof(out->unit) - 1);
    out->unit[sizeof(out->unit) - 1] = '\0';
    return true;
}

static void epoch_us_to_date(int64_t epoch_us, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_us / 1000000);
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(out, out_len, "%Y-%m-%d", &tm_info);
}

typedef void (*log_row_cb_t)(const log_row_t *row, void *ctx);

// Chunk-reads the current log through sd_storage_read() (never the whole
// file at once — SD-card side of a 4 MB-flash, no-PSRAM chip) and invokes
// cb() for every row whose date matches. Buffers are static: this walks
// over ~600 B of state regardless of file size, and keeps it off the
// httpd worker's stack. Always reads logger_get_current_path() — whatever
// name the log-manager screen has it on right now, not a name baked in
// here — so a rename there doesn't leave this quietly reading a stale path.
static void walk_log_rows_for_date(const char *date, log_row_cb_t cb, void *ctx)
{
    static char chunk[HISTORY_READ_CHUNK];
    static char line[HISTORY_LINE_MAX];
    size_t line_len = 0;
    size_t offset = 0;
    bool header_skipped = false;

    while (1) {
        size_t out_len = 0;
        esp_err_t err = sd_storage_read(logger_get_current_path(), offset, chunk, sizeof(chunk), &out_len);
        if (err != ESP_OK || out_len == 0) {
            break;
        }
        offset += out_len;

        for (size_t i = 0; i < out_len; i++) {
            char c = chunk[i];
            if (c != '\n') {
                if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                }
                continue;
            }
            line[line_len] = '\0';
            line_len = 0;

            if (!header_skipped) {
                header_skipped = true;
                continue;
            }

            log_row_t row;
            if (!parse_log_line(line, &row)) {
                continue;
            }
            char row_date[11];
            epoch_us_to_date(row.ts_epoch_us, row_date, sizeof(row_date));
            if (strcmp(row_date, date) == 0) {
                cb(&row, ctx);
            }
        }
    }
}

typedef struct {
    bool found;
} find_ctx_t;

static void find_cb(const log_row_t *row, void *ctx)
{
    ((find_ctx_t *)ctx)->found = true;
}

typedef struct {
    httpd_req_t *req;
    bool first;
} stream_ctx_t;

static void stream_cb(const log_row_t *row, void *ctx)
{
    stream_ctx_t *sc = (stream_ctx_t *)ctx;
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%s{\"ts_epoch\":%lld,\"channel\":\"%s\",\"value\":%.4f,\"unit\":\"%s\"}",
                        sc->first ? "" : ",", (long long)(row->ts_epoch_us / 1000000),
                        row->channel, row->value, row->unit);
    if (len > 0 && (size_t)len < sizeof(buf)) {
        httpd_resp_send_chunk(sc->req, buf, len);
        sc->first = false;
    }
}

static bool get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        return false;
    }
    char *query = malloc(qlen);
    if (query == NULL) {
        return false;
    }
    bool ok = false;
    if (httpd_req_get_url_query_str(req, query, qlen) == ESP_OK) {
        ok = (httpd_query_key_value(query, key, out, out_len) == ESP_OK);
    }
    free(query);
    return ok;
}

// A bare filename, relative to sd_storage's mount point — rejects any
// component that could walk outside it ('/' or "..") rather than trusting
// the query string against the filesystem directly.
static bool is_safe_filename(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strstr(name, "..") != NULL || strchr(name, '/') != NULL) {
        return false;
    }
    return true;
}

// GET /api/history?date=YYYY-MM-DD — reads the whole file twice (once to
// check for a match, so a 404 can still be sent before any bytes go out;
// once to stream matches as they're found). Simple, and log.csv is a
// hobby-scale file, not something worth a single-pass buffering scheme for.
static esp_err_t api_history_handler(httpd_req_t *req)
{
    if (!s_time_synced) {
        return send_json_error(req, "503 Service Unavailable", "time not synced yet (SNTP)");
    }

    char date[16];
    if (!get_query_param(req, "date", date, sizeof(date))) {
        return send_json_error(req, HTTPD_400, "?date=YYYY-MM-DD is required");
    }

    if (!sd_storage_exists(logger_get_current_path())) {
        return send_json_error(req, HTTPD_404, "no log file yet");
    }

    find_ctx_t fc = { .found = false };
    walk_log_rows_for_date(date, find_cb, &fc);
    if (!fc.found) {
        return send_json_error(req, HTTPD_404, "no rows for that date");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    stream_ctx_t sc = { .req = req, .first = true };
    walk_log_rows_for_date(date, stream_cb, &sc);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// GET /api/logs — every file on the card (name, size) via
// sd_storage_list_dir(), plus total/free space from the same
// sd_storage_get_info() call the on-device sd_info screen makes, so the
// web dashboard can show a real explorer instead of one static download
// link tied to whatever file logger happened to be on at build time.
static esp_err_t api_logs_handler(httpd_req_t *req)
{
    sd_storage_dir_entry_t entries[16];
    size_t n = sd_storage_list_dir(NULL, entries, 16);

    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "size", (double)entries[i].size_bytes);
        cJSON_AddItemToArray(files, item);
    }
    cJSON_AddItemToObject(root, "files", files);

    sd_storage_info_t info;
    if (sd_storage_get_info(&info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "total_bytes", (double)info.total_bytes);
        cJSON_AddNumberToObject(root, "free_bytes", (double)info.free_bytes);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /download?file=<name> — any file from /api/logs' listing, not just
// whatever logger is currently writing to. Rejects a missing or unsafe
// filename before ever touching sd_storage with it.
static esp_err_t download_handler(httpd_req_t *req)
{
    char file[SD_STORAGE_NAME_LEN];
    if (!get_query_param(req, "file", file, sizeof(file)) || !is_safe_filename(file)) {
        return send_json_error(req, HTTPD_400, "?file=<name> is required");
    }
    if (!sd_storage_exists(file)) {
        return send_json_error(req, HTTPD_404, "no such file");
    }

    httpd_resp_set_type(req, "text/csv");
    char disposition[SD_STORAGE_NAME_LEN + 32];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", file);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    static char chunk[1024];
    size_t offset = 0;
    while (1) {
        size_t out_len = 0;
        esp_err_t err = sd_storage_read(file, offset, chunk, sizeof(chunk), &out_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "download: sd_storage_read failed: %s", esp_err_to_name(err));
            break;
        }
        if (out_len == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, out_len) != ESP_OK) {
            break; // client went away mid-download
        }
        offset += out_len;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = s_http_port;
    config.stack_size = 6144;
    config.core_id = WIFI_BRINGUP_CORE;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/api/data", .method = HTTP_GET, .handler = api_data_handler },
        { .uri = "/api/history", .method = HTTP_GET, .handler = api_history_handler },
        { .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler },
        { .uri = "/download", .method = HTTP_GET, .handler = download_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_httpd, &routes[i]);
    }

    ESP_LOGI(TAG, "httpd started on port %u", s_http_port);
    return ESP_OK;
}

esp_err_t web_server_init(const web_server_config_t *config)
{
    s_http_port = (config != NULL && config->http_port != 0) ? config->http_port : 80;

    BaseType_t ok = xTaskCreatePinnedToCore(wifi_bringup_task, "web_bringup",
                                             WIFI_BRINGUP_STACK, NULL, WIFI_BRINGUP_PRIO,
                                             NULL, WIFI_BRINGUP_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void web_server_get_status(web_server_status_t *out)
{
    out->wifi_mode = s_wifi_mode;
    strncpy(out->ssid, s_wifi_ssid, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    strncpy(out->ip, s_wifi_ip, sizeof(out->ip) - 1);
    out->ip[sizeof(out->ip) - 1] = '\0';
    out->time_synced = s_time_synced;
    out->time_source = s_time_source;
}

void web_server_sync_ntp_now(void)
{
    // esp_netif_sntp_start() restarts the client if it's already running
    // (it was started once from start_sntp() after the first STA
    // connect), forcing a fresh attempt right now instead of waiting for
    // its own poll interval. sntp_sync_cb() fires and sets
    // WEB_SERVER_TIME_NTP the same way it would on any other sync.
    esp_netif_sntp_start();
}

bool web_server_get_wall_clock(time_t *out_epoch_utc)
{
    if (!s_time_synced) {
        return false;
    }
    int64_t epoch_us = esp_timer_get_time() + s_boot_epoch_offset_us;
    *out_epoch_utc = (time_t)(epoch_us / 1000000);
    return true;
}

void web_server_set_wall_clock(time_t epoch_utc)
{
    int64_t epoch_us = (int64_t)epoch_utc * 1000000LL;
    s_boot_epoch_offset_us = epoch_us - esp_timer_get_time();
    s_time_synced = true;
    s_time_source = WEB_SERVER_TIME_MANUAL;
    ESP_LOGI(TAG, "wall clock set manually");
}

static void forget_wifi_task(void *arg)
{
    // Give the caller's UI time to show feedback ("Forgetting...") before
    // esp_restart() tears the whole board down — same reasoning as
    // wifi_provision.c's reboot_task after a portal form submit.
    vTaskDelay(pdMS_TO_TICKS(600));
    wifi_provision_clear();
    esp_restart();
}

void web_server_forget_wifi(void)
{
    xTaskCreate(forget_wifi_task, "wifi_forget", 2048, NULL, 5, NULL);
}
