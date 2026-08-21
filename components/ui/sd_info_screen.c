#include "sd_info_screen.h"

#include <stdbool.h>
#include <stdio.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "sd_storage.h"

static lv_obj_t *s_sd_info_label;
static lv_timer_t *s_sd_info_refresh_timer;
static bool s_sd_info_screen_active;

static void sd_info_back_cb(lv_event_t *e)
{
    s_sd_info_screen_active = false;
    screen_pop();
}

static void sd_info_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_sd_info_screen_active) {
        return;
    }

    char buf[192];
    if (!sd_storage_is_mounted()) {
        snprintf(buf, sizeof(buf), "SD card: not mounted");
    } else {
        sd_storage_info_t info;
        if (sd_storage_get_info(&info) == ESP_OK) {
            uint64_t cap_mb = info.card_capacity_bytes / (1024 * 1024);
            uint64_t total_mb = info.total_bytes / (1024 * 1024);
            uint64_t free_mb = info.free_bytes / (1024 * 1024);
            uint64_t used_mb = total_mb > free_mb ? total_mb - free_mb : 0;
            snprintf(buf, sizeof(buf),
                     "SD card: mounted\nType: %s\nCapacity: %llu MB\nUsed: %llu MB\nFree: %llu MB",
                     info.card_type,
                     (unsigned long long)cap_mb,
                     (unsigned long long)used_mb,
                     (unsigned long long)free_mb);
        } else {
            snprintf(buf, sizeof(buf), "SD card: mounted\n(info read failed)");
        }
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_sd_info_label, buf);
    lvgl_port_unlock();
}

void sd_info_screen_create(void)
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
    lv_obj_add_event_cb(btn_back, sd_info_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "SD Card");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height and SCROLLABLE cleared — see
    // measurement_screen.c's note on why this matters.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    s_sd_info_label = lv_label_create(body);
    lv_label_set_text(s_sd_info_label, "Loading...");
    lv_obj_set_style_text_color(s_sd_info_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(s_sd_info_label, 6, 0);

    s_sd_info_screen_active = true;

    lvgl_port_unlock();

    if (s_sd_info_refresh_timer == NULL) {
        s_sd_info_refresh_timer = lv_timer_create(sd_info_refresh_timer_cb, 2000, NULL);
    }
}
