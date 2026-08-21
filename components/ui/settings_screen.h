#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// A flat menu into Time, WiFi, Manage Logs, and SD card — each its own
// screen now (time_settings_screen.c/wifi_settings_screen.c/
// log_manager_screen.c/sd_info_screen.c). Reached from the dashboard's
// Settings button.
void settings_screen_create(void);

#ifdef __cplusplus
}
#endif
