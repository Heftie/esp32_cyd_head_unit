#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// WiFi status (mode, SSID, IP, time-sync) via web_server's status
// snapshot, a "forget this network" action, and a way into the touch
// diagnostics screen. Reached from the dashboard's Settings button.
void settings_screen_create(void);

#ifdef __cplusplus
}
#endif
