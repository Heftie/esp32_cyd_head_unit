#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// 0 until an SNTP sync (or a manual set) has landed. Once set, any
// boot-relative esp_timer_get_time() timestamp converts to wall-clock
// epoch microseconds by adding this offset — used both by api_data_handler
// below and, via web_server_convert_boot_time_us(), by logger.c to decide
// what to actually write into a CSV row (nothing, until this is set).
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

// --- Timezone (display-only; storage and web_server_get_wall_clock() stay UTC) --

// DST rules taken from the tz database's own POSIX-TZ backward-compat
// strings (transition weekday/week/hour, not a fixed date) so each one
// keeps applying correctly year over year rather than needing an update.
const web_server_timezone_t web_server_timezones[] = {
    { "Berlin (CET/CEST)",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "UTC",                   "UTC0" },
    { "London (GMT/BST)",      "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Helsinki (EET/EEST)",   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Moscow (MSK)",          "MSK-3" },
    { "New York (EST/EDT)",    "EST5EDT,M3.2.0,M11.1.0" },
    { "Los Angeles (PST/PDT)", "PST8PDT,M3.2.0,M11.1.0" },
    { "Dubai (GST)",           "GST-4" },
    { "New Delhi (IST)",       "IST-5:30" },
    { "Shanghai (CST)",        "CST-8" },
    { "Tokyo (JST)",           "JST-9" },
    { "Sydney (AEST/AEDT)",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
const size_t web_server_timezone_count = sizeof(web_server_timezones) / sizeof(web_server_timezones[0]);

#define NVS_TZ_NAMESPACE "tz_cfg"
#define NVS_TZ_KEY       "tz_idx"

static size_t s_timezone_index = 0;

static void apply_timezone(size_t index)
{
    s_timezone_index = index;
    setenv("TZ", web_server_timezones[index].posix_tz, 1);
    tzset();
}

// Loads a persisted timezone choice (default index 0 — Berlin — on first
// boot, when nothing's been saved yet) and applies it immediately. Unlike
// the wall clock itself, this doesn't need Wi-Fi or SNTP — the TZ rule
// only changes how an already-known UTC time gets displayed — so
// web_server_init() calls this straight away rather than waiting on
// wifi_bringup_task.
static void load_timezone_from_nvs(void)
{
    uint8_t idx = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_TZ_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_TZ_KEY, &idx);
        nvs_close(h);
    }
    if (idx >= web_server_timezone_count) {
        idx = 0;
    }
    apply_timezone(idx);
    ESP_LOGI(TAG, "timezone: %s", web_server_timezones[idx].name);
}

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
    // Without this, a browser is free to cache this page indefinitely —
    // it's a single static response with no Last-Modified/ETag for it to
    // revalidate against, and this HTML is the entire app (CSS + JS
    // inlined, no separate asset files). A tab left open across a
    // firmware update just keeps running whatever JS it already loaded,
    // same as any page would; this at least makes a *fresh* load/reload
    // always get the current version instead of a browser-cached one.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
        // 0 (Unix epoch) if the file was written before any wall clock
        // ever landed this boot — see sd_storage_list_dir()'s comment.
        // Sent regardless rather than filtered to null, since the
        // dashboard's sort-by-modified still needs a comparable value
        // even for those rows.
        cJSON_AddNumberToObject(item, "mtime", (double)entries[i].mtime);
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

// GET /api/time — the device's own wall clock (UTC epoch seconds) and
// whether it's actually synced yet. Lets the dashboard's "Default name"
// button build the same YYYYMMDD_HHMMSS_log.csv pattern tiles' Start
// button and log_manager's own Default-name button do (see
// log_naming_default_filename()), from the device's clock rather than
// the browser's — which could be skewed from it, especially for a device
// that's had no NTP sync yet and is only running off a manual set.
static esp_err_t api_time_handler(httpd_req_t *req)
{
    time_t now;
    bool synced = web_server_get_wall_clock(&now);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "synced", synced);
    if (synced) {
        cJSON_AddNumberToObject(root, "epoch", (double)now);
    } else {
        cJSON_AddNullToObject(root, "epoch");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/log/status — logger's current state, the web equivalent of what
// the on-device log-manager screen's status line shows ("Logging to: X
// (running/stopped)").
static esp_err_t api_log_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", logger_is_running());
    cJSON_AddStringToObject(root, "current_file", logger_get_current_path());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/log/start?name=<name> — the web equivalent of log-manager's
// name field + Use button. name is optional: omitted (or empty) just
// resumes whatever file logger is already set to, same as pressing Use
// with an empty textarea on-device. A given name doesn't need to already
// exist — logger_start() writes a fresh header row for one that doesn't.
static esp_err_t api_log_start_handler(httpd_req_t *req)
{
    char name[SD_STORAGE_NAME_LEN];
    bool has_name = get_query_param(req, "name", name, sizeof(name));
    if (has_name && !is_safe_filename(name)) {
        return send_json_error(req, HTTPD_400, "invalid name");
    }

    esp_err_t err = logger_start(has_name ? name : NULL);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return api_log_status_handler(req);
}

// POST /api/log/stop — the web equivalent of... there's no direct
// on-device equivalent, actually; the tiles screen's Start/Stop toggle is
// the closest analog. Pauses the logger task; logger_stop() is always
// safe to call even if already stopped.
static esp_err_t api_log_stop_handler(httpd_req_t *req)
{
    logger_stop();
    return api_log_status_handler(req);
}

// POST /api/log/delete?file=<name> — same rule as log-manager's per-row
// Delete: refuses the file logger is actively writing to, since erasing
// it out from under an open logging session is a footgun with no upside
// over just stopping first.
static esp_err_t api_log_delete_handler(httpd_req_t *req)
{
    char file[SD_STORAGE_NAME_LEN];
    if (!get_query_param(req, "file", file, sizeof(file)) || !is_safe_filename(file)) {
        return send_json_error(req, HTTPD_400, "?file=<name> is required");
    }
    if (!sd_storage_exists(file)) {
        return send_json_error(req, HTTPD_404, "no such file");
    }
    if (logger_is_running() && strcmp(file, logger_get_current_path()) == 0) {
        return send_json_error(req, "409 Conflict", "file is actively being logged to — stop first");
    }

    esp_err_t err = sd_storage_erase(file);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "deleted", true);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/api/data", .method = HTTP_GET, .handler = api_data_handler },
        { .uri = "/api/time", .method = HTTP_GET, .handler = api_time_handler },
        { .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler },
        { .uri = "/api/log/status", .method = HTTP_GET, .handler = api_log_status_handler },
        { .uri = "/api/log/start", .method = HTTP_POST, .handler = api_log_start_handler },
        { .uri = "/api/log/stop", .method = HTTP_POST, .handler = api_log_stop_handler },
        { .uri = "/api/log/delete", .method = HTTP_POST, .handler = api_log_delete_handler },
        { .uri = "/download", .method = HTTP_GET, .handler = download_handler },
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = s_http_port;
    config.stack_size = 6144;
    config.core_id = WIFI_BRINGUP_CORE;
    // HTTPD_DEFAULT_CONFIG()'s max_uri_handlers is 8 — one short of
    // routes[] below once the log-control endpoints landed, which failed
    // silently (httpd_register_uri_handler()'s return value went
    // unchecked) until a "no slots left" log line gave it away. Sized
    // with headroom rather than an exact match, so the next route added
    // here doesn't silently repeat this.
    config.max_uri_handlers = 16;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_httpd, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register route: %s", routes[i].uri);
        }
    }

    ESP_LOGI(TAG, "httpd started on port %u", s_http_port);
    return ESP_OK;
}

esp_err_t web_server_init(const web_server_config_t *config)
{
    s_http_port = (config != NULL && config->http_port != 0) ? config->http_port : 80;

    // NVS + timezone don't depend on Wi-Fi, so set them up here rather
    // than inside wifi_bringup_task — the on-device clock should read
    // correctly in local time even before, or without, a network.
    // nvs_flash_init() is safe to call again from wifi_bringup_task below:
    // it's a no-op once the default partition is already initialized.
    ESP_ERROR_CHECK(nvs_flash_init());
    load_timezone_from_nvs();

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

bool web_server_convert_boot_time_us(int64_t boot_time_us, int64_t *out_epoch_us)
{
    if (!s_time_synced) {
        return false;
    }
    *out_epoch_us = boot_time_us + s_boot_epoch_offset_us;
    return true;
}

void web_server_set_wall_clock(time_t epoch_utc)
{
    int64_t epoch_us = (int64_t)epoch_utc * 1000000LL;
    s_boot_epoch_offset_us = epoch_us - esp_timer_get_time();
    s_time_synced = true;
    s_time_source = WEB_SERVER_TIME_MANUAL;

    // An SNTP sync calls settimeofday() itself, internally, before ever
    // reaching sntp_sync_cb() — this is the manual-set equivalent of
    // that. Without it, libc's own time()/gettimeofday() stay wherever
    // they defaulted to (this board has no RTC, so effectively unset),
    // even though web_server's own s_boot_epoch_offset_us above is
    // correct — and FATFS's get_fattime() (components/fatfs/diskio/
    // diskio.c, upstream ESP-IDF, not something this repo owns) reads
    // time() directly for every file's last-modified stamp. Skipping
    // this left every file saved during a manual-time-only session
    // (no NTP at all) stamped with whatever time() defaulted to instead
    // of the actual wall-clock time.
    struct timeval tv = { .tv_sec = epoch_utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "wall clock set manually");
}

esp_err_t web_server_set_timezone(size_t index)
{
    if (index >= web_server_timezone_count) {
        return ESP_ERR_INVALID_ARG;
    }
    apply_timezone(index);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_TZ_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_TZ_KEY, (uint8_t)index);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "timezone set to %s", web_server_timezones[index].name);
    return err;
}

size_t web_server_get_timezone_index(void)
{
    return s_timezone_index;
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
