#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// SD card status: mounted/not, card type, capacity, used/free space, via
// sd_storage_is_mounted()/sd_storage_get_info(), plus a button into
// log_manager. Reached from the settings screen's "SD card" button.
void sd_info_screen_create(void);

#ifdef __cplusplus
}
#endif
