#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Registers every screen (tiles, settings, measurement, set_time,
// sd_info, log_manager, graph, graph_config) and shows the home screen.
// Call once from app_main(), after the LVGL display/touch bring-up
// (lv_scr_act() must already be valid) and after data_hub_init() — the
// only other hard prerequisite, since data_hub_list_channels() takes a
// mutex that's NULL until then. Deliberately called before
// uart_link/sd_storage/logger/web_server bring-up: every screen reads
// those through its own refresh timer via plain static-default reads
// (mcu_present=false, mounted=false, running=false, wall clock unset)
// that are safe to see before that subsystem's own _init() has run, so
// the screen shows up immediately rather than waiting behind whichever
// of those is slowest or fails outright — web_server's WiFi connect
// alone can take up to 20s before falling back to the setup AP.
void ui_init(void);

#ifdef __cplusplus
}
#endif
