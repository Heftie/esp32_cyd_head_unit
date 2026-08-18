#include "wifi_provision.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>

static const char *TAG = "wifi_provision";

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define DNS_PORT       53
#define DNS_PACKET_MAX 512 // room for a typical query plus our appended answer record

static httpd_handle_t s_portal_httpd;

extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[] asm("_binary_portal_html_end");

// --- NVS credential storage ---------------------------------------------

bool wifi_provision_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t ssid_sz = ssid_len;
    esp_err_t ssid_err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz);

    size_t pass_sz = pass_len;
    if (nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz) != ESP_OK) {
        pass[0] = '\0'; // no stored password — treat as an open network
    }

    nvs_close(h);
    return (ssid_err == ESP_OK && ssid[0] != '\0');
}

esp_err_t wifi_provision_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// --- DNS hijack: answer every query with the AP's own IP ----------------
// Standard minimal captive-portal trick: don't bother parsing the question,
// just flip the header to "response" and append one A-record answer that
// points (via a name-compression pointer back to offset 12) at whatever
// was asked, resolving it to our AP address. Good enough for the handful
// of probe domains phones/laptops actually check.

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static void dns_hijack_task(void *arg)
{
    uint32_t ap_ip = (uint32_t)(uintptr_t)arg; // already network byte order

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive DNS responder up on :%d", DNS_PORT);

    uint8_t buf[DNS_PACKET_MAX];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *hdr = (dns_header_t *)buf;
        hdr->flags = htons(0x8180); // response, recursion available, no error
        hdr->ancount = htons(1);
        hdr->nscount = 0;
        hdr->arcount = 0;

        uint8_t *answer = buf + len;
        answer[0] = 0xC0;
        answer[1] = 0x0C; // name: pointer to the question at offset 12
        answer[2] = 0x00;
        answer[3] = 0x01; // type A
        answer[4] = 0x00;
        answer[5] = 0x01; // class IN
        answer[6] = 0x00;
        answer[7] = 0x00;
        answer[8] = 0x00;
        answer[9] = 0x3C; // TTL 60s
        answer[10] = 0x00;
        answer[11] = 0x04; // RDLENGTH 4
        memcpy(&answer[12], &ap_ip, 4);

        sendto(sock, buf, len + 12 + 4, 0, (struct sockaddr *)&from, from_len);
    }
}

// --- Portal HTTP handlers -------------------------------------------------

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)portal_html_start, portal_html_end - portal_html_start);
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s != '\0') {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // give the HTTP response time to actually reach the client
    esp_restart();
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
        return ESP_FAIL;
    }
    httpd_query_key_value(body, "password", pass, sizeof(pass)); // optional — open network

    url_decode(ssid);
    url_decode(pass);

    if (wifi_provision_save(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save credentials");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "credentials saved for \"%s\", rebooting", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><html><body style=\"font-family:sans-serif;background:#10141b;"
        "color:#e6e9ef;padding:24px\"><h1>Saved</h1>"
        "<p>Restarting and connecting now\xe2\x80\xa6</p></body></html>");

    xTaskCreate(reboot_task, "wifi_prov_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Wildcard fallback — most OSes probe a handful of well-known paths
// (/generate_204, /hotspot-detect.html, /ncsi.txt, ...) to decide whether
// to pop the "sign in to network" prompt. Answering all of them with the
// same setup page is what makes that happen instead of a 404.
static esp_err_t portal_catchall_handler(httpd_req_t *req)
{
    return portal_root_handler(req);
}

esp_err_t wifi_provision_start_ap(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    int n = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "CYD-Setup-%02X%02X", mac[4], mac[5]);
    ap_cfg.ap.ssid_len = (uint8_t)n;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    ESP_LOGI(TAG, "provisioning AP \"%s\" up (open) — connect and browse to any address, or " IPSTR,
             (char *)ap_cfg.ap.ssid, IP2STR(&ip_info.ip));

    xTaskCreate(dns_hijack_task, "dns_hijack", 3072, (void *)(uintptr_t)ip_info.ip.addr, 5, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;

    err = httpd_start(&s_portal_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = portal_root_handler };
    static const httpd_uri_t save_uri = { .uri = "/api/wifi", .method = HTTP_POST, .handler = portal_save_handler };
    static const httpd_uri_t catchall_uri = { .uri = "/*", .method = HTTP_GET, .handler = portal_catchall_handler };
    httpd_register_uri_handler(s_portal_httpd, &root_uri);
    httpd_register_uri_handler(s_portal_httpd, &save_uri);
    httpd_register_uri_handler(s_portal_httpd, &catchall_uri);

    return ESP_OK;
}
