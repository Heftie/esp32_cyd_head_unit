#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-device log management: set the filename logger writes to next,
// browse what's currently on the card (name + size), delete individual
// files, or wipe the whole card. Reached from the sd_info screen's
// "Manage logs" button.
void log_manager_screen_create(void);

#ifdef __cplusplus
}
#endif
