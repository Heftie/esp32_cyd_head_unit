#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads stored WiFi credentials from NVS. Returns false if none are stored
// yet. pass may come back as an empty string for an open network.
bool wifi_provision_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// Persists credentials to NVS (namespace "wifi_cfg").
esp_err_t wifi_provision_save(const char *ssid, const char *pass);

// Brings up a SoftAP ("CYD-Setup-XXXX", open), a DNS responder that
// resolves every query to the AP's own IP (so most phones/laptops surface
// the setup page automatically, the way any other captive portal does),
// and a small HTTP server with the setup form. Submitting it saves
// credentials via wifi_provision_save() and reboots — the next boot picks
// them up via wifi_provision_load() and tries STA again.
//
// Returns once the AP/portal are up; does not block waiting for the form
// to be submitted.
esp_err_t wifi_provision_start_ap(void);

#ifdef __cplusplus
}
#endif
