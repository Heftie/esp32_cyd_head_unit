#include "time_settings_screen.h"

#include <stdbool.h>
#include <stdio.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "web_server.h"

static lv_obj_t *s_time_status_label;
static lv_obj_t *s_time_sync_btn;
static lv_timer_t *s_time_refresh_timer;
static bool s_time_settings_screen_active;

static void time_settings_back_cb(lv_event_t *e)
{
    s_time_settings_screen_active = false;
    screen_pop();
}

static void time_settings_set_time_cb(lv_event_t *e)
{
    s_time_settings_screen_active = false;
    screen_push("set_time");
}

// No confirm step needed here, unlike Forget network — this only ever
// forces a fresh attempt; it can't make the clock any less correct than
// it already is, and it's a no-op if there's no network path to an NTP
// server (same as at boot).
static void time_settings_sync_ntp_cb(lv_event_t *e)
{
    web_server_sync_ntp_now();
}

static void time_settings_timezone_cb(lv_event_t *e)
{
    s_time_settings_screen_active = false;
    screen_push("timezone");
}

static void time_settings_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_time_settings_screen_active) {
        return;
    }

    web_server_status_t status;
    web_server_get_status(&status);

    const char *time_str = "not set";
    if (status.time_source == WEB_SERVER_TIME_NTP) {
        time_str = "NTP";
    } else if (status.time_source == WEB_SERVER_TIME_MANUAL) {
        time_str = "manual";
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "Source: %s", time_str);

    lvgl_port_lock(0);

    lv_label_set_text(s_time_status_label, buf);

    // Sync only means something with an actual network — nothing to sync
    // while running the setup AP.
    bool sta_connected = (status.wifi_mode == WEB_SERVER_WIFI_STA);
    if (sta_connected) {
        lv_obj_add_flag(s_time_sync_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_time_sync_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(s_time_sync_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_time_sync_btn, LV_OPA_50, 0);
    }

    lvgl_port_unlock();
}

static void time_settings_create_menu_button(lv_obj_t *parent, lv_obj_t **btn_out,
                                              const char *label, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, lv_pct(100), 40);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);
    if (btn_out != NULL) {
        *btn_out = btn;
    }
}

void time_settings_screen_create(void)
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
    lv_obj_add_event_cb(btn_back, time_settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Time");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height and SCROLLABLE cleared — see
    // measurement_screen.c's note on why this matters.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    s_time_status_label = lv_label_create(body);
    lv_label_set_text(s_time_status_label, "Loading status...");
    lv_obj_set_style_text_color(s_time_status_label, lv_color_hex(0x888888), 0);

    time_settings_create_menu_button(body, NULL, "Set time", time_settings_set_time_cb);
    time_settings_create_menu_button(body, &s_time_sync_btn, "Sync NTP", time_settings_sync_ntp_cb);
    time_settings_create_menu_button(body, NULL, "Timezone", time_settings_timezone_cb);

    s_time_settings_screen_active = true;

    lvgl_port_unlock();

    if (s_time_refresh_timer == NULL) {
        s_time_refresh_timer = lv_timer_create(time_settings_refresh_timer_cb, 1000, NULL);
    }
}
