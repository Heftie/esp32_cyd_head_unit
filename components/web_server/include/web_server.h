#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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

typedef enum {
    WEB_SERVER_TIME_UNSET = 0, // no wall clock yet — this board has no
                                // battery-backed RTC, so this is the
                                // state on every boot until one of the
                                // two sources below lands
    WEB_SERVER_TIME_NTP,       // set by sntp_sync_cb()
    WEB_SERVER_TIME_MANUAL,    // set by web_server_set_wall_clock()
} web_server_time_source_t;

typedef struct {
    web_server_wifi_mode_t wifi_mode;
    char ssid[33]; // STA: the network joined; AP: the SoftAP's own SSID
    char ip[16];   // dotted-quad, empty until known
    bool time_synced;
    web_server_time_source_t time_source;
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
//   GET  /api/history?date=   the current log's rows for one day (YYYY-MM-DD), as JSON
//   GET  /api/logs            every file on the card (name, size) + total/free space, as JSON
//   GET  /download?file=      raw contents of one named file from /api/logs
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

// Manually sets the wall clock, as a fallback for when SNTP never lands —
// this board has no battery-backed RTC crystal, so the wall-clock offset
// normally comes only from SNTP and resets to unset on every boot; if the
// network has no path to an NTP server, /api/history stays permanently
// 503 with no other way to recover. epoch_utc is UTC seconds since 1970.
// Takes effect immediately, the same way an SNTP sync would, and reports
// as WEB_SERVER_TIME_MANUAL in web_server_get_status() afterward.
void web_server_set_wall_clock(time_t epoch_utc);

// Forces a fresh SNTP sync attempt right now, instead of waiting for the
// client's own poll interval — the way back to WEB_SERVER_TIME_NTP after
// a manual set, once the network can actually reach an NTP server again.
// No-op-ish if there's no route out (SoftAP mode, or STA with no
// internet): the attempt just times out silently, same as it would at
// boot, and the status stays whatever it already was.
void web_server_sync_ntp_now(void);

// Current wall-clock time (UTC seconds since 1970), from whichever
// source last set it (NTP or manual) — see web_server_status_t's
// time_source. Returns false if neither has landed yet.
bool web_server_get_wall_clock(time_t *out_epoch_utc);

#ifdef __cplusplus
}
#endif
