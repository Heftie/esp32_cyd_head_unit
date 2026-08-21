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

typedef struct {
    const char *name;     // display label for UI pickers, e.g. "Berlin (CET/CEST)"
    const char *posix_tz; // POSIX TZ rule string for setenv("TZ", ...) + tzset()
} web_server_timezone_t;

// Curated list of selectable timezones for UI pickers (e.g. a settings
// screen roller). Index 0 — Europe/Berlin — is the default applied at
// boot, since this firmware's primary deployment is in Germany.
extern const web_server_timezone_t web_server_timezones[];
extern const size_t web_server_timezone_count;

// Kicks off Wi-Fi station bring-up (SSID/password from Kconfig — see
// Kconfig.projbuild), SNTP wall-clock sync, mDNS, and the HTTP server, all
// from a task pinned to core 1 (the Wi-Fi driver's own task defaults to
// core 0). Returns once that task is created — it does not block waiting
// for the network to come up, so a missing/wrong network doesn't hold up
// the rest of app_main. Serves:
//
//   GET  /                    embedded dashboard (index.html)
//   GET  /api/data            current data_hub channel values, as JSON
//   GET  /api/time            { synced, epoch } — device wall clock, for the dashboard's Default-name button
//   GET  /api/logs            every file on the card (name, size, mtime) + total/free space, as JSON
//   GET  /api/log/status      { running, current_file } — logger's current state
//   POST /api/log/start?name= (re)starts logging; name optional, resumes current file if omitted
//   POST /api/log/stop        pauses logging
//   POST /api/log/delete?file= deletes one file; refuses the file logger is actively writing
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
// normally comes only from SNTP and resets to unset on every boot; with
// neither source landed, logger holds off writing anything at all (see
// logger_config_t.convert_boot_time_fn) rather than logging a meaningless
// boot-relative time. epoch_utc is UTC seconds since 1970. Takes effect
// immediately, the same way an SNTP sync would, and reports as
// WEB_SERVER_TIME_MANUAL in web_server_get_status() afterward.
void web_server_set_wall_clock(time_t epoch_utc);

// Converts a boot-relative esp_timer_get_time() timestamp into wall-clock
// UTC microseconds, using whichever offset web_server_get_wall_clock()
// itself would currently use (NTP or manual). Returns false if neither
// source has landed yet. Exposed as a plain, stateless function — not a
// component-level dependency — specifically so logger.c can convert its
// own boot-relative data_hub timestamps without introducing a
// logger<->web_server circular dependency: main.c wires this in as
// logger_config_t.convert_boot_time_fn instead of logger.c ever
// including this header.
bool web_server_convert_boot_time_us(int64_t boot_time_us, int64_t *out_epoch_us);

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

// Applies web_server_timezones[index] (setenv("TZ", ...) + tzset()) and
// persists the choice to NVS so it survives reboot. This only changes how
// a UTC time is *displayed* as local struct tm (e.g. via localtime_r) —
// web_server_get_wall_clock()'s return value, logger's stored CSV
// timestamps, and set_time's manual entry all stay UTC regardless, so log
// files and history stay unambiguous across a timezone change. Returns
// ESP_ERR_INVALID_ARG if index is out of range.
esp_err_t web_server_set_timezone(size_t index);

// Index into web_server_timezones[] of the currently applied timezone —
// for a UI to preselect the right roller entry. Defaults to 0 (Berlin)
// until web_server_init() loads a persisted choice from NVS, or forever
// if none was ever saved.
size_t web_server_get_timezone_index(void);

#ifdef __cplusplus
}
#endif
