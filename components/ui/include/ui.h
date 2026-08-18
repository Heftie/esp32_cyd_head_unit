#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Registers every screen (tiles, settings, measurement, set_time,
// sd_info) and shows the home screen. Call once from app_main(), after
// the LVGL display/touch bring-up — lv_scr_act() must already be valid —
// and after data_hub_init()/web_server_init(), since the tile and
// settings screens start reading those on their own refresh timers as
// soon as they're shown.
void ui_init(void);

#ifdef __cplusplus
}
#endif
