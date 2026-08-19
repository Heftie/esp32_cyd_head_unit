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
} logger_config_t;

// Creates the logger task (wakes every flush_interval_ms, walks data_hub's
// channels, and appends any samples not yet written to the current log
// path as CSV rows: "timestamp_us,channel,value,unit") and immediately
// starts it logging to config->log_path — equivalent to logger_init()
// followed by logger_start(NULL). Requires sd_storage to already be
// mounted.
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
