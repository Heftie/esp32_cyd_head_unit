#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Manual wall-clock entry (UTC) — the fallback for when SNTP never lands.
// This board has no battery-backed RTC crystal, so web_server's wall
// clock normally comes only from SNTP and resets to unset on every boot;
// if the network has no path to an NTP server, this is the only way to
// give it one at all. Reached from the settings screen's "Set time"
// button.
void set_time_screen_create(void);

#ifdef __cplusplus
}
#endif
