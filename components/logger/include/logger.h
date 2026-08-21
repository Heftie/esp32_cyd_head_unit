#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *log_path;        // relative to sd_storage's mount point, e.g. "log.csv"
    uint32_t flush_interval_ms;  // 0 = default (5000)
    uint32_t max_file_bytes;     // 0 = default (5 MB); log_path rotates to "<log_path>.1" past this

    // Converts a data_hub sample's boot-relative esp_timer_get_time()
    // timestamp into wall-clock UTC microseconds. logger has no wall-clock
    // concept of its own (that's web_server's SNTP/manual-set job, since
    // it owns the NTP client) — the caller wires this in from main.c as a
    // plain function pointer (web_server_convert_boot_time_us) rather
    // than logger.c including web_server.h directly, which would create a
    // logger<->web_server circular component dependency (web_server
    // already depends on logger for logger_get_current_path()). Required;
    // logger_init() fails if this is NULL.
    bool (*convert_boot_time_fn)(int64_t boot_time_us, int64_t *out_epoch_us);
} logger_config_t;

// Creates the logger task (wakes every flush_interval_ms, walks data_hub's
// channels, and appends any samples not yet written to the current log
// path as CSV rows: "timestamp_epoch_us,channel,value,unit"), idle until a
// caller explicitly calls logger_start() — this does NOT start logging
// on its own, so nothing gets written to the card until the user presses
// Start/Use in the UI. Requires sd_storage to already be mounted.
//
// A sample is only ever written once convert_boot_time_fn can actually
// resolve it to a real UTC time — before that (no NTP sync and no manual
// set yet), flushes are a no-op and nothing is lost from data_hub's ring
// buffer beyond its own normal eviction, so logging just silently starts
// working the moment a wall clock lands rather than ever writing a
// meaningless "seconds since boot" row.
//
// The current file grows without bound otherwise, so once it crosses
// max_file_bytes the task rotates it: the current file becomes
// "<path>.1" (overwriting any previous backup) and a fresh file starts
// with just the header row. Single generation, not a full logrotate
// scheme — this is a hobby-scale device, not something worth keeping N
// generations for.
esp_err_t logger_init(const logger_config_t *config);

// Pauses logging — the task keeps running (just idles) so logger_start()
// can resume without recreating anything. Safe to call even if already
// stopped.
void logger_stop(void);

// (Re)starts logging. If `name` is non-NULL and differs from the current
// log path, switches to that file instead — writing its header row if it
// doesn't exist yet, and resetting the "already logged" bookmarks that
// only make sense for the previous file (same reasoning as the rotation
// this triggers past max_file_bytes). Pass NULL to just resume the
// current file where it left off. Requires sd_storage to already be
// mounted.
esp_err_t logger_start(const char *name);

// True if the logger task is currently flushing data_hub to a file
// rather than idling after logger_stop().
bool logger_is_running(void);

// The filename logger is currently set to write to (relative to
// sd_storage's mount point) — whatever logger_init()/logger_start() last
// set it to, regardless of whether logging is currently running or
// stopped. Empty string if neither has ever run. Points at internal
// static storage; valid for the life of the program, not to be freed.
const char *logger_get_current_path(void);

#ifdef __cplusplus
}
#endif
