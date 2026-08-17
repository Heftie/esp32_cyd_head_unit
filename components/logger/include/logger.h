#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *log_path;       // relative to sd_storage's mount point, e.g. "log.csv"
    uint32_t flush_interval_ms; // 0 = default (5000)
} logger_config_t;

// Starts a task that wakes every flush_interval_ms, walks data_hub's
// channels, and appends any samples not yet written to log_path as CSV
// rows ("timestamp_us,channel,value,unit"). Requires sd_storage to already
// be mounted.
esp_err_t logger_init(const logger_config_t *config);

#ifdef __cplusplus
}
#endif
