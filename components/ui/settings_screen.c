#include "settings_screen.h"

#include <stdbool.h>
#include <stdio.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "web_server.h"

static lv_obj_t *s_settings_status_label;
static lv_obj_t *s_settings_sync_btn;
static lv_obj_t *s_settings_forget_btn;
static lv_obj_t *s_settings_forget_label;
static lv_timer_t *s_settings_refresh_timer;
static lv_timer_t *s_settings_forget_disarm_timer;
static bool s_settings_screen_active;
static bool s_settings_forget_armed;

static void settings_back_cb(lv_event_t *e)
{
    s_settings_screen_active = false;
    screen_pop();
}

static void settings_set_time_cb(lv_event_t *e)
{
    s_settings_screen_active = false;
    screen_push("set_time");
}

// No confirm step needed here, unlike Forget network — this only ever
// forces a fresh attempt; it can't make the clock any less correct than
// it already is, and it's a no-op if there's no network path to an NTP
// server (same as at boot).
static void settings_sync_ntp_cb(lv_event_t *e)
{
    web_server_sync_ntp_now();
}

static void settings_sd_info_cb(lv_event_t *e)
{
    s_settings_screen_active = false;
    screen_push("sd_info");
}

static void settings_forget_disarm_cb(lv_timer_t *timer)
{
    s_settings_forget_armed = false;
    s_settings_forget_disarm_timer = NULL;
    lv_label_set_text(s_settings_forget_label, "Forget network");
}

// First tap arms a 3s confirmation window instead of acting immediately —
// this reboots the device and drops it off its network, so a single
// accidental tap next to Back shouldn't be enough to trigger it.
static void settings_forget_cb(lv_event_t *e)
{
    if (!s_settings_forget_armed) {
        s_settings_forget_armed = true;
        lv_label_set_text(s_settings_forget_label, "Tap again to confirm");
        s_settings_forget_disarm_timer = lv_timer_create(settings_forget_disarm_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_settings_forget_disarm_timer, 1);
        return;
    }

    if (s_settings_forget_disarm_timer != NULL) {
        lv_timer_del(s_settings_forget_disarm_timer);
        s_settings_forget_disarm_timer = NULL;
    }
    lv_label_set_text(s_settings_forget_label, "Forgetting...");
    web_server_forget_wifi();
}

static void settings_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_settings_screen_active) {
        return;
    }

    web_server_status_t status;
    web_server_get_status(&status);

    const char *mode_str = "Connecting...";
    if (status.wifi_mode == WEB_SERVER_WIFI_STA) {
        mode_str = "Connected";
    } else if (status.wifi_mode == WEB_SERVER_WIFI_AP) {
        mode_str = "Access point (setup mode)";
    }

    const char *time_str = "not set";
    if (status.time_source == WEB_SERVER_TIME_NTP) {
        time_str = "NTP";
    } else if (status.time_source == WEB_SERVER_TIME_MANUAL) {
        time_str = "manual";
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "WiFi: %s\nSSID: %s\nIP: %s  Time: %s",
             mode_str,
             status.ssid[0] ? status.ssid : "--",
             status.ip[0] ? status.ip : "--",
             time_str);

    lvgl_port_lock(0);

    lv_label_set_text(s_settings_status_label, buf);

    // Syncing and forgetting both need an actual network to mean
    // anything — nothing to sync or forget while running the setup AP.
    bool sta_connected = (status.wifi_mode == WEB_SERVER_WIFI_STA);
    if (sta_connected) {
        lv_obj_add_flag(s_settings_sync_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_sync_btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_settings_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_forget_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(s_settings_sync_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_sync_btn, LV_OPA_50, 0);
        lv_obj_clear_flag(s_settings_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_forget_btn, LV_OPA_50, 0);
    }

    lvgl_port_unlock();
}

static lv_obj_t *settings_create_nav_button(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 36);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);
    return btn;
}

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

    s_settings_status_label = lv_label_create(body);
    lv_label_set_text(s_settings_status_label, "Loading status...");
    lv_obj_set_style_text_color(s_settings_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(s_settings_status_label, 4, 0);

    // Set time / Sync NTP / SD card are all plain navigation or a
    // one-shot retry — none of them have lasting consequences, so they
    // share one row. Forget network gets its own row below: it's the one
    // action here that actually changes something (drops the network).
    lv_obj_t *nav_row = lv_obj_create(body);
    lv_obj_remove_style_all(nav_row);
    lv_obj_set_size(nav_row, lv_pct(100), 36);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(nav_row, 8, 0);

    settings_create_nav_button(nav_row, "Set time", settings_set_time_cb);
    s_settings_sync_btn = settings_create_nav_button(nav_row, "Sync NTP", settings_sync_ntp_cb);
    settings_create_nav_button(nav_row, "SD card", settings_sd_info_cb);

    s_settings_forget_btn = lv_button_create(body);
    lv_obj_set_size(s_settings_forget_btn, lv_pct(100), 40);
    lv_obj_add_event_cb(s_settings_forget_btn, settings_forget_cb, LV_EVENT_CLICKED, NULL);
    s_settings_forget_label = lv_label_create(s_settings_forget_btn);
    lv_label_set_text(s_settings_forget_label, "Forget network");
    lv_obj_center(s_settings_forget_label);

    s_settings_forget_armed = false;
    s_settings_screen_active = true;

    lvgl_port_unlock();

    if (s_settings_refresh_timer == NULL) {
        s_settings_refresh_timer = lv_timer_create(settings_refresh_timer_cb, 1000, NULL);
    }
}
