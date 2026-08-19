#pragma once

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

// Starts a task that wakes every flush_interval_ms, walks data_hub's
// channels, and appends any samples not yet written to log_path as CSV
// rows ("timestamp_us,channel,value,unit"). Requires sd_storage to already
// be mounted.
//
// log_path grows without bound otherwise, so once it crosses
// max_file_bytes the task rotates it: the current file becomes
// "<log_path>.1" (overwriting any previous backup) and a fresh log_path
// starts with just the header row. Single generation, not a full
// logrotate scheme — this is a hobby-scale device, not something worth
// keeping N generations for.
esp_err_t logger_init(const logger_config_t *config);

#ifdef __cplusplus
}
#endif
