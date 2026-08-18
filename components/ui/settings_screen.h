#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// WiFi status (mode, SSID, IP, time-sync source) via web_server's status
// snapshot, a "forget this network" action, a "sync NTP now" retry, and
// buttons into the manual clock-set screen and the SD card info screen.
// Reached from the dashboard's Settings button.
void settings_screen_create(void);

#ifdef __cplusplus
}
#endif
