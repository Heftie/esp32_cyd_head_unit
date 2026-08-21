#include "wifi_settings_screen.h"

#include <stdbool.h>
#include <stdio.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "web_server.h"

static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_wifi_forget_btn;
static lv_obj_t *s_wifi_forget_label;
static lv_timer_t *s_wifi_refresh_timer;
static lv_timer_t *s_wifi_forget_disarm_timer;
static bool s_wifi_settings_screen_active;
static bool s_wifi_forget_armed;

static void wifi_settings_back_cb(lv_event_t *e)
{
    s_wifi_settings_screen_active = false;
    screen_pop();
}

static void wifi_settings_forget_disarm_cb(lv_timer_t *timer)
{
    s_wifi_forget_armed = false;
    s_wifi_forget_disarm_timer = NULL;
    lv_label_set_text(s_wifi_forget_label, "Forget network");
}

// First tap arms a 3s confirmation window instead of acting immediately —
// this reboots the device and drops it off its network, so a single
// accidental tap next to Back shouldn't be enough to trigger it.
static void wifi_settings_forget_cb(lv_event_t *e)
{
    if (!s_wifi_forget_armed) {
        s_wifi_forget_armed = true;
        lv_label_set_text(s_wifi_forget_label, "Tap again to confirm");
        s_wifi_forget_disarm_timer = lv_timer_create(wifi_settings_forget_disarm_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_wifi_forget_disarm_timer, 1);
        return;
    }

    if (s_wifi_forget_disarm_timer != NULL) {
        lv_timer_del(s_wifi_forget_disarm_timer);
        s_wifi_forget_disarm_timer = NULL;
    }
    lv_label_set_text(s_wifi_forget_label, "Forgetting...");
    web_server_forget_wifi();
}

static void wifi_settings_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_wifi_settings_screen_active) {
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

    char buf[128];
    snprintf(buf, sizeof(buf), "WiFi: %s\nSSID: %s\nIP: %s",
             mode_str,
             status.ssid[0] ? status.ssid : "--",
             status.ip[0] ? status.ip : "--");

    lvgl_port_lock(0);

    lv_label_set_text(s_wifi_status_label, buf);

    // Forgetting needs an actual network to mean anything — nothing to
    // forget while already running the setup AP.
    bool sta_connected = (status.wifi_mode == WEB_SERVER_WIFI_STA);
    if (sta_connected) {
        lv_obj_add_flag(s_wifi_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_wifi_forget_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(s_wifi_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_wifi_forget_btn, LV_OPA_50, 0);
    }

    lvgl_port_unlock();
}

void wifi_settings_screen_create(void)
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
    lv_obj_add_event_cb(btn_back, wifi_settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "WiFi");
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

    s_wifi_status_label = lv_label_create(body);
    lv_label_set_text(s_wifi_status_label, "Loading status...");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(s_wifi_status_label, 4, 0);

    s_wifi_forget_btn = lv_button_create(body);
    lv_obj_set_size(s_wifi_forget_btn, lv_pct(100), 40);
    lv_obj_add_event_cb(s_wifi_forget_btn, wifi_settings_forget_cb, LV_EVENT_CLICKED, NULL);
    s_wifi_forget_label = lv_label_create(s_wifi_forget_btn);
    lv_label_set_text(s_wifi_forget_label, "Forget network");
    lv_obj_center(s_wifi_forget_label);

    s_wifi_forget_armed = false;
    s_wifi_settings_screen_active = true;

    lvgl_port_unlock();

    if (s_wifi_refresh_timer == NULL) {
        s_wifi_refresh_timer = lv_timer_create(wifi_settings_refresh_timer_cb, 1000, NULL);
    }
}
