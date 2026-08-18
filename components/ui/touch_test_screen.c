#include "touch_test_screen.h"

#include <stdio.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"

typedef struct {
    lv_obj_t *area;
    lv_obj_t *label;
} touch_test_ui_t;

static touch_test_ui_t touch_test_ui;

static void touch_test_draw_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lvgl_port_lock(0);

    lv_obj_t *dot = lv_obj_create(touch_test_ui.area);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_pos(dot, point.x - 3, point.y - 3);

    char buf[32];
    snprintf(buf, sizeof(buf), "X:%ld  Y:%ld", (long)point.x, (long)point.y);
    lv_label_set_text(touch_test_ui.label, buf);

    lvgl_port_unlock();
}

static void touch_test_clear_cb(lv_event_t *e)
{
    lvgl_port_lock(0);
    lv_obj_clean(touch_test_ui.area);
    lvgl_port_unlock();
}

static void touch_test_back_cb(lv_event_t *e)
{
    screen_pop();
}

void touch_test_screen_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    // lv_obj_clean() only removes children — the screen object itself is
    // reused across screens, so tiles_screen_create()'s flex layout/padding
    // would otherwise leak in here and turn this into a scrolling page.
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    touch_test_ui.area = lv_obj_create(scr);
    lv_obj_remove_style_all(touch_test_ui.area);
    lv_obj_set_size(touch_test_ui.area, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(touch_test_ui.area, 0, 0);
    lv_obj_add_flag(touch_test_ui.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_test_ui.area, touch_test_draw_cb, LV_EVENT_PRESSING, NULL);

    touch_test_ui.label = lv_label_create(scr);
    lv_label_set_text(touch_test_ui.label, "Touch to test");
    lv_obj_set_style_text_color(touch_test_ui.label, lv_color_white(), 0);
    lv_obj_align(touch_test_ui.label, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *btn_clear = lv_button_create(scr);
    lv_obj_set_size(btn_clear, 80, 35);
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_event_cb(btn_clear, touch_test_clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, "Clear");
    lv_obj_center(lbl_clear);

    lv_obj_t *btn_back = lv_button_create(scr);
    lv_obj_set_size(btn_back, 80, 35);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_add_event_cb(btn_back, touch_test_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lvgl_port_unlock();
}
