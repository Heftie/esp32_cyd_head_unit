#include "settings_screen.h"

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"

static void settings_back_cb(lv_event_t *e)
{
    screen_pop();
}

static void settings_open_time_cb(lv_event_t *e)
{
    screen_push("time_settings");
}

static void settings_open_wifi_cb(lv_event_t *e)
{
    screen_push("wifi_settings");
}

static void settings_open_log_manager_cb(lv_event_t *e)
{
    screen_push("log_manager");
}

static void settings_open_sd_info_cb(lv_event_t *e)
{
    screen_push("sd_info");
}

static void settings_create_menu_button(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, lv_pct(100), 40);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);
}

// A pure menu now — Time (set_time/Sync NTP/timezone), WiFi (status +
// Forget network), Manage Logs, and SD card info each moved out into
// their own screen (see time_settings_screen.c/wifi_settings_screen.c/
// log_manager_screen.c/sd_info_screen.c) so this top level stays a flat
// list of destinations rather than a growing pile of unrelated status
// text and buttons sharing one screen.
void settings_screen_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 34);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_gap(header, 10, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 68, 32);
    lv_obj_add_event_cb(btn_back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height (240 screen - 34 header) rather than
    // flex_grow(1), and SCROLLABLE cleared — same fix applied here as
    // measurement_screen.c after that screen's content silently outgrew
    // its container.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    settings_create_menu_button(body, "Time", settings_open_time_cb);
    settings_create_menu_button(body, "WiFi", settings_open_wifi_cb);
    settings_create_menu_button(body, "Manage Logs", settings_open_log_manager_cb);
    settings_create_menu_button(body, "SD card", settings_open_sd_info_cb);

    lvgl_port_unlock();
}
