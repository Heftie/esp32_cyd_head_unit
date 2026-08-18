#include "measurement_screen.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "data_hub.h"
#include "screen_nav.h"

// min/max/avg are accumulated here rather than read out of data_hub's ring
// buffer — that buffer is only DATA_HUB_HISTORY_LEN (32) samples deep,
// deliberately just enough to bridge to logger's next flush (see
// data_hub.h), and keeps overwriting itself regardless of this screen's
// Start/Reset state. Stats always start fresh on entry, matching this
// component's rebuild-from-scratch-on-entry convention (see
// tiles_screen_create()) — this screen doesn't try to resume a run left
// going from a previous visit.

typedef struct {
    bool running;
    bool has_sample;
    float min;
    float max;
    double sum;
    uint32_t count;
} measurement_stats_t;

// Which channel to show — set by measurement_screen_set_channel() just
// before a caller (tiles_screen.c) pushes here, since screen_push() only
// takes a name and this file has exactly one instance of the screen.
static char s_measurement_channel[DATA_HUB_NAME_LEN];

static measurement_stats_t s_measurement_stats;
static lv_obj_t *s_measurement_title_label;
static lv_obj_t *s_measurement_value_label;
static lv_obj_t *s_measurement_min_label;
static lv_obj_t *s_measurement_max_label;
static lv_obj_t *s_measurement_avg_label;
static lv_obj_t *s_measurement_run_label;
static lv_timer_t *s_measurement_refresh_timer;
static bool s_measurement_screen_active;

void measurement_screen_set_channel(const char *name)
{
    strncpy(s_measurement_channel, name, sizeof(s_measurement_channel) - 1);
    s_measurement_channel[sizeof(s_measurement_channel) - 1] = '\0';
}

static void measurement_back_cb(lv_event_t *e)
{
    s_measurement_screen_active = false;
    screen_pop();
}

static void measurement_reset_cb(lv_event_t *e)
{
    s_measurement_stats.has_sample = false;
    s_measurement_stats.min = 0.0f;
    s_measurement_stats.max = 0.0f;
    s_measurement_stats.sum = 0.0;
    s_measurement_stats.count = 0;
}

static void measurement_run_cb(lv_event_t *e)
{
    s_measurement_stats.running = !s_measurement_stats.running;
    lv_label_set_text(s_measurement_run_label, s_measurement_stats.running ? "Stop" : "Start");
}

static void measurement_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_measurement_screen_active) {
        return;
    }

    data_hub_sample_t sample;
    bool have_value = data_hub_get_latest(s_measurement_channel, &sample);

    if (have_value && s_measurement_stats.running) {
        if (!s_measurement_stats.has_sample) {
            s_measurement_stats.min = sample.value;
            s_measurement_stats.max = sample.value;
            s_measurement_stats.has_sample = true;
        } else if (sample.value < s_measurement_stats.min) {
            s_measurement_stats.min = sample.value;
        } else if (sample.value > s_measurement_stats.max) {
            s_measurement_stats.max = sample.value;
        }
        s_measurement_stats.sum += sample.value;
        s_measurement_stats.count++;
    }

    lvgl_port_lock(0);

    char value_buf[32];
    if (have_value) {
        snprintf(value_buf, sizeof(value_buf), "%.3f %s", sample.value, sample.unit);
    } else {
        snprintf(value_buf, sizeof(value_buf), "--");
    }
    lv_label_set_text(s_measurement_value_label, value_buf);

    // Three short labels rather than one long formatted string — a single
    // "min X  max Y  avg Z (n=N)" line is easy to make wider than the
    // screen without noticing, and a wrapped label silently grows taller
    // than expected, which is exactly what pushed this screen's bottom
    // half off-screen before (see the layout note in
    // measurement_screen_create()).
    char buf[16];
    if (s_measurement_stats.has_sample) {
        float avg = (float)(s_measurement_stats.sum / s_measurement_stats.count);
        snprintf(buf, sizeof(buf), "%.2f", s_measurement_stats.min);
        lv_label_set_text(s_measurement_min_label, buf);
        snprintf(buf, sizeof(buf), "%.2f", s_measurement_stats.max);
        lv_label_set_text(s_measurement_max_label, buf);
        snprintf(buf, sizeof(buf), "%.2f", avg);
        lv_label_set_text(s_measurement_avg_label, buf);
    } else {
        lv_label_set_text(s_measurement_min_label, "--");
        lv_label_set_text(s_measurement_max_label, "--");
        lv_label_set_text(s_measurement_avg_label, "--");
    }

    lvgl_port_unlock();
}

// One MIN/MAX/AVG column: a small caption over a value, both short enough
// that they can never wrap regardless of channel unit or magnitude.
// Returns the value label so the caller can update it on each refresh.
static lv_obj_t *measurement_create_stat_column(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, 44);
    lv_obj_set_flex_grow(col, 1); // split the row's actual width evenly across 3 columns
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x888888), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);

    return val;
}

void measurement_screen_create(void)
{
    lvgl_port_lock(0);

    // Every container below gets an explicit size — this screen's first
    // draft left the button row unsized and let one long stats string wrap
    // to extra lines, both of which silently pushed content past the
    // bottom of the display and made the whole screen scroll. Disabling
    // LV_OBJ_FLAG_SCROLLABLE on scr/body turns any future budget mistake
    // here into visible clipping instead of an invisible scroll, which is
    // easier to spot and fix.
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 34);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_gap(header, 10, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 68, 32);
    lv_obj_add_event_cb(btn_back, measurement_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    s_measurement_title_label = lv_label_create(header);
    lv_label_set_text(s_measurement_title_label, s_measurement_channel);
    lv_obj_set_style_text_color(s_measurement_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_measurement_title_label, &lv_font_montserrat_20, 0);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206); // 240 screen - 34 header, explicit rather than flex_grow
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(body, 6, 0);

    s_measurement_value_label = lv_label_create(body);
    lv_label_set_text(s_measurement_value_label, "--");
    lv_obj_set_style_text_color(s_measurement_value_label, lv_color_hex(0x00e08a), 0);
    lv_obj_set_style_text_font(s_measurement_value_label, &lv_font_montserrat_40, 0);

    lv_obj_t *stats_row = lv_obj_create(body);
    lv_obj_remove_style_all(stats_row);
    lv_obj_set_size(stats_row, lv_pct(100), 44);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_measurement_min_label = measurement_create_stat_column(stats_row, "MIN");
    s_measurement_max_label = measurement_create_stat_column(stats_row, "MAX");
    s_measurement_avg_label = measurement_create_stat_column(stats_row, "AVG");

    lv_obj_t *btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, 12, 0);

    lv_obj_t *btn_run = lv_button_create(btn_row);
    lv_obj_set_size(btn_run, 100, 40);
    lv_obj_add_event_cb(btn_run, measurement_run_cb, LV_EVENT_CLICKED, NULL);
    s_measurement_run_label = lv_label_create(btn_run);
    lv_label_set_text(s_measurement_run_label, "Start");
    lv_obj_center(s_measurement_run_label);

    lv_obj_t *btn_reset = lv_button_create(btn_row);
    lv_obj_set_size(btn_reset, 100, 40);
    lv_obj_add_event_cb(btn_reset, measurement_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "Reset");
    lv_obj_center(lbl_reset);

    s_measurement_stats.running = false;
    s_measurement_stats.has_sample = false;
    s_measurement_stats.min = 0.0f;
    s_measurement_stats.max = 0.0f;
    s_measurement_stats.sum = 0.0;
    s_measurement_stats.count = 0;
    s_measurement_screen_active = true;

    lvgl_port_unlock();

    if (s_measurement_refresh_timer == NULL) {
        s_measurement_refresh_timer = lv_timer_create(measurement_refresh_timer_cb, 500, NULL);
    }
}
