#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Current time source (NTP/manual/not set) plus buttons into the manual
// clock-set screen, a "sync NTP now" retry, and the timezone screen —
// everything to do with what time it is and how it's displayed, in one
// place. Reached from the settings screen's "Time" button.
void time_settings_screen_create(void);

#ifdef __cplusplus
}
#endif
