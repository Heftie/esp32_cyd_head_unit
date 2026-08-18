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

// Erases stored SSID/password from NVS. The next wifi_provision_load()
// call returns false, so a caller that reboots after this falls straight
// into wifi_provision_start_ap() again. Intended for an on-device "forget
// this network" action — otherwise the only way off a bad network is
// finding the captive portal again, which requires already being off it.
esp_err_t wifi_provision_clear(void);

// Brings up a SoftAP ("CYD-Setup-XXXX", open), a DNS responder that
// resolves every query to the AP's own IP (so most phones/laptops surface
// the setup page automatically, the way any other captive portal does),
// and a small HTTP server with the setup form. Submitting it saves
// credentials via wifi_provision_save() and reboots — the next boot picks
// them up via wifi_provision_load() and tries STA again.
//
// On success, and if non-NULL, copies the AP's SSID and dotted-quad IP
// into ssid_out/ip_out — for a caller that wants to report them (e.g. a
// settings screen). Pass NULL/0 for either to skip it.
//
// Returns once the AP/portal are up; does not block waiting for the form
// to be submitted.
esp_err_t wifi_provision_start_ap(char *ssid_out, size_t ssid_out_len, char *ip_out, size_t ip_out_len);

#ifdef __cplusplus
}
#endif
