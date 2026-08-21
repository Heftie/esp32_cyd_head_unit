#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// A single lv_roller over web_server_timezones[], preselected to
// web_server_get_timezone_index(), plus an Apply button that calls
// web_server_set_timezone() and a Back button that discards the change.
// Only affects how times are displayed (tiles' clock, a future web
// picker) — never what's stored in logs or web_server's wall clock.
// Reached from the settings screen.
void timezone_screen_create(void);

#ifdef __cplusplus
}
#endif
