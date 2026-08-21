#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// WiFi status (mode/SSID/IP, from web_server's status snapshot, polled on
// its own timer) and a "Forget network" action, gated behind a
// tap-to-arm/tap-to-confirm sequence since it's a one-way trip off the
// current network. Reached from the settings screen's "WiFi" button.
void wifi_settings_screen_create(void);

#ifdef __cplusplus
}
#endif
