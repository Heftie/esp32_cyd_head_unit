#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t http_port; // 0 = default (80)
} web_server_config_t;

typedef enum {
    WEB_SERVER_WIFI_CONNECTING = 0, // bring-up hasn't landed on STA or AP yet
    WEB_SERVER_WIFI_STA,            // joined the stored network
    WEB_SERVER_WIFI_AP,             // stored network unreachable/absent — running the setup AP
} web_server_wifi_mode_t;

typedef struct {
    web_server_wifi_mode_t wifi_mode;
    char ssid[33]; // STA: the network joined; AP: the SoftAP's own SSID
    char ip[16];   // dotted-quad, empty until known
    bool time_synced;
} web_server_status_t;

// Kicks off Wi-Fi station bring-up (SSID/password from Kconfig — see
// Kconfig.projbuild), SNTP wall-clock sync, mDNS, and the HTTP server, all
// from a task pinned to core 1 (the Wi-Fi driver's own task defaults to
// core 0). Returns once that task is created — it does not block waiting
// for the network to come up, so a missing/wrong network doesn't hold up
// the rest of app_main. Serves:
//
//   GET  /                    embedded dashboard (index.html)
//   GET  /api/data            current data_hub channel values, as JSON
//   GET  /api/history?date=   log.csv rows for one day (YYYY-MM-DD), as JSON
//   GET  /download            raw log.csv
esp_err_t web_server_init(const web_server_config_t *config);

// Snapshot of current WiFi/time state, for UI use (e.g. a settings
// screen). Cheap and safe to call from any task/timer; the underlying
// fields are set once per bring-up outcome from the WiFi event/bring-up
// task and read here without a lock — same convention this file already
// uses for its boot_epoch_offset/time_synced state.
void web_server_get_status(web_server_status_t *out);

// Erases stored WiFi credentials (see wifi_provision.h) and reboots. The
// next boot finds none and falls straight into the SoftAP setup flow.
// Intended for an on-device "forget this network" UI action.
void web_server_forget_wifi(void);

#ifdef __cplusplus
}
#endif
