#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t http_port; // 0 = default (80)
} web_server_config_t;

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

#ifdef __cplusplus
}
#endif
