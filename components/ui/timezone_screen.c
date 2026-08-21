#include "timezone_screen.h"

#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "web_server.h"

static lv_obj_t *s_tz_roller;
static lv_obj_t *s_tz_status_label;

// Built once per screen entry from web_server_timezones[] rather than a
// static string literal — the option list lives in web_server.h/.c, not
// duplicated here, so adding an entry there is the only place that has
// to change.
static char s_tz_options[512];

static void timezone_build_options(void)
{
    size_t off = 0;
    for (size_t i = 0; i < web_server_timezone_count; i++) {
        int len = snprintf(s_tz_options + off, sizeof(s_tz_options) - off,
                            "%s%s", (i == 0) ? "" : "\n", web_server_timezones[i].name);
        if (len < 0 || (size_t)len >= sizeof(s_tz_options) - off) {
            break;
        }
        off += (size_t)len;
    }
}

static void timezone_back_cb(lv_event_t *e)
{
    screen_pop();
}

static void timezone_apply_cb(lv_event_t *e)
{
    uint32_t idx = lv_roller_get_selected(s_tz_roller);
    esp_err_t err = web_server_set_timezone((size_t)idx);
    if (err == ESP_OK) {
        lv_label_set_text(s_tz_status_label, "Saved");
        screen_pop(); // back to settings
    } else {
        lv_label_set_text(s_tz_status_label, "Failed to save");
    }
}

void timezone_screen_create(void)
{
    timezone_build_options();

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
    lv_obj_add_event_cb(btn_back, timezone_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Time Zone");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height and SCROLLABLE cleared — same fix as every
    // other screen here (see measurement_screen.c's note on why).
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    s_tz_roller = lv_roller_create(body);
    lv_roller_set_options(s_tz_roller, s_tz_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(s_tz_roller, lv_pct(100));
    lv_obj_set_flex_grow(s_tz_roller, 1);
    lv_roller_set_selected(s_tz_roller, (uint32_t)web_server_get_timezone_index(), LV_ANIM_OFF);

    s_tz_status_label = lv_label_create(body);
    lv_label_set_text(s_tz_status_label, "");
    lv_obj_set_style_text_color(s_tz_status_label, lv_color_hex(0x888888), 0);

    lv_obj_t *btn_apply = lv_button_create(body);
    lv_obj_set_size(btn_apply, lv_pct(100), 40);
    lv_obj_add_event_cb(btn_apply, timezone_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_apply = lv_label_create(btn_apply);
    lv_label_set_text(lbl_apply, "Apply");
    lv_obj_center(lbl_apply);

    lvgl_port_unlock();
}
