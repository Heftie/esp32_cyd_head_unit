#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-device log management: set the filename logger writes to next (a
// "Default" button restores the YYYYMMDD_HHMMSS_log.csv pattern tiles'
// Start button uses, for after typing something custom), browse what's
// currently on the card (name + size, newest-first by mtime — no header
// clicks to change it, unlike the web dashboard's sortable Logs card;
// this list is short enough on a 240x320 screen that newest-first is
// just always the useful order), delete individual files, or wipe the
// whole card. Reached from the settings screen's "Manage Logs" button.
void log_manager_screen_create(void);

#ifdef __cplusplus
}
#endif
