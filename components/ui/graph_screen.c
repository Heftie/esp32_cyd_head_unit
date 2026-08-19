#include "graph_screen.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "data_hub.h"
#include "screen_nav.h"

// This board has no PSRAM (see data_hub.h's own DRAM-budget note), so the
// chart's point buffer is capped rather than sized to whatever tick/span
// the picker asks for. lv_chart stores one int32_t per point per series —
// 240 points is roughly this display's pixel width (past which extra
// points buy no visible resolution anyway) and costs under 1 KB for the
// one series this screen ever has.
#define GRAPH_MAX_POINTS  240
#define GRAPH_VALUE_SCALE 100 // chart values are fixed-point, value*100, for 2 decimal places of resolution

static const uint32_t s_tick_seconds[] = { 1, 2, 5, 10, 30, 60 };
static const char *s_tick_options = "1s\n2s\n5s\n10s\n30s\n1min";

static const uint32_t s_span_seconds[] = { 60, 300, 600, 1800, 3600, 10800, 21600, 43200, 86400 };
static const char *s_span_options = "1min\n5min\n10min\n30min\n1h\n3h\n6h\n12h\n24h";

// Persist across screen re-entries (this component's screens rebuild
// from scratch each time they're shown — see tiles_screen.c) so a config
// change sticks and a plain Back-and-forth doesn't lose it.
static char s_selected_channel[DATA_HUB_NAME_LEN];
static uint32_t s_tick_idx;
static uint32_t s_span_idx;

// --- Chart view ("graph") --------------------------------------------

static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t *s_range_label;
static lv_obj_t *s_min_label;
static lv_obj_t *s_max_label;
static lv_obj_t *s_now_label;
static lv_timer_t *s_sample_timer;
static bool s_graph_screen_active;
static bool s_has_sample;
static float s_min_value;
static float s_max_value;

// span_s already accounts for tick_s * point-count clamping — see
// graph_compute_effective() — so this is purely display formatting, not
// another place the clamp math could drift out of sync with it.
static void format_duration(uint32_t seconds, char *out, size_t out_len)
{
    if (seconds < 60) {
        snprintf(out, out_len, "%us", (unsigned)seconds);
    } else if (seconds < 3600) {
        snprintf(out, out_len, "%umin", (unsigned)(seconds / 60));
    } else {
        snprintf(out, out_len, "%uh", (unsigned)(seconds / 3600));
    }
}

// The one place the RAM-budget math actually lives — every label that
// mentions point count or effective span, in either screen, computes it
// by calling this rather than repeating the division/clamp inline.
static void graph_compute_effective(uint32_t tick_s, uint32_t span_s,
                                     uint32_t *out_points, uint32_t *out_span_s, bool *out_capped)
{
    uint32_t points = span_s / (tick_s ? tick_s : 1);
    if (points == 0) {
        points = 1;
    }
    if (points > GRAPH_MAX_POINTS) {
        points = GRAPH_MAX_POINTS;
        *out_capped = true;
    } else {
        *out_capped = false;
    }
    *out_points = points;
    *out_span_s = points * tick_s;
}

static void graph_back_cb(lv_event_t *e)
{
    s_graph_screen_active = false;
    screen_pop();
}

static void graph_config_open_cb(lv_event_t *e)
{
    s_graph_screen_active = false;
    screen_push("graph_config");
}

static void graph_sample_timer_cb(lv_timer_t *timer)
{
    if (!s_graph_screen_active) {
        return;
    }

    data_hub_sample_t sample;
    if (!data_hub_get_latest(s_selected_channel, &sample)) {
        return;
    }

    if (!s_has_sample) {
        s_min_value = sample.value;
        s_max_value = sample.value;
        s_has_sample = true;
    } else if (sample.value < s_min_value) {
        s_min_value = sample.value;
    } else if (sample.value > s_max_value) {
        s_max_value = sample.value;
    }

    lvgl_port_lock(0);

    lv_chart_set_next_value(s_chart, s_series, (int32_t)(sample.value * GRAPH_VALUE_SCALE));

    int32_t range_min = (int32_t)(s_min_value * GRAPH_VALUE_SCALE);
    int32_t range_max = (int32_t)(s_max_value * GRAPH_VALUE_SCALE);
    if (range_min == range_max) {
        range_max = range_min + GRAPH_VALUE_SCALE; // avoid a zero-height axis on the very first sample
    }
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, range_min, range_max);

    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", s_min_value);
    lv_label_set_text(s_min_label, buf);
    snprintf(buf, sizeof(buf), "%.2f", s_max_value);
    lv_label_set_text(s_max_label, buf);
    snprintf(buf, sizeof(buf), "%.2f %s", sample.value, sample.unit);
    lv_label_set_text(s_now_label, buf);

    lvgl_port_unlock();
}

static lv_obj_t *graph_create_stat_column(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, 40);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x888888), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xcccccc), 0);

    return val;
}

void graph_screen_set_channel(const char *name)
{
    strncpy(s_selected_channel, name, sizeof(s_selected_channel) - 1);
    s_selected_channel[sizeof(s_selected_channel) - 1] = '\0';
}

void graph_screen_create(void)
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
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_gap(header, 8, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 60, 32);
    lv_obj_add_event_cb(btn_back, graph_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, s_selected_channel);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_flex_grow(title, 1);

    lv_obj_t *btn_cfg = lv_button_create(header);
    lv_obj_set_size(btn_cfg, 50, 32);
    lv_obj_add_event_cb(btn_cfg, graph_config_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cfg = lv_label_create(btn_cfg);
    lv_label_set_text(lbl_cfg, "Cfg");
    lv_obj_center(lbl_cfg);

    uint32_t tick_s = s_tick_seconds[s_tick_idx];
    uint32_t span_s = s_span_seconds[s_span_idx];
    uint32_t points;
    uint32_t eff_span_s;
    bool capped;
    graph_compute_effective(tick_s, span_s, &points, &eff_span_s, &capped);

    // Explicit body height and SCROLLABLE cleared — see
    // measurement_screen.c's note on why this matters.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_gap(body, 8, 0);

    s_range_label = lv_label_create(body);
    char tick_buf[16];
    format_duration(tick_s, tick_buf, sizeof(tick_buf));
    char span_buf[16];
    format_duration(eff_span_s, span_buf, sizeof(span_buf));
    char range_buf[80];
    snprintf(range_buf, sizeof(range_buf), "%u pts, %s tick, %s span%s",
             (unsigned)points, tick_buf, span_buf, capped ? " (capped)" : "");
    lv_label_set_text(s_range_label, range_buf);
    lv_obj_set_style_text_color(s_range_label, lv_color_hex(0x888888), 0);

    lv_obj_t *stats_row = lv_obj_create(body);
    lv_obj_remove_style_all(stats_row);
    lv_obj_set_size(stats_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_min_label = graph_create_stat_column(stats_row, "MIN");
    s_max_label = graph_create_stat_column(stats_row, "MAX");
    s_now_label = graph_create_stat_column(stats_row, "NOW");

    s_chart = lv_chart_create(body);
    lv_obj_set_width(s_chart, lv_pct(100));
    lv_obj_set_flex_grow(s_chart, 1);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, points);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(0x00e08a), LV_CHART_AXIS_PRIMARY_Y);

    s_has_sample = false;
    s_graph_screen_active = true;

    lvgl_port_unlock();

    // Always torn down and recreated at this (possibly new) tick
    // rate — unlike this file's other timers, the period itself can
    // change between visits via graph_config_screen, so there's no
    // fixed period a one-time "create if NULL" could reuse.
    if (s_sample_timer != NULL) {
        lv_timer_del(s_sample_timer);
    }
    s_sample_timer = lv_timer_create(graph_sample_timer_cb, tick_s * 1000, NULL);
}

// --- Config view ("graph_config") -------------------------------------

#define GRAPH_MAX_CHANNELS DATA_HUB_MAX_CHANNELS

static char s_channel_names[GRAPH_MAX_CHANNELS][DATA_HUB_NAME_LEN];
static char s_channel_options[GRAPH_MAX_CHANNELS * (DATA_HUB_NAME_LEN + 1)];
static size_t s_channel_count;

static lv_obj_t *s_channel_roller;
static lv_obj_t *s_tick_roller;
static lv_obj_t *s_span_roller;
static lv_obj_t *s_config_effective_label;

static void graph_config_build_channel_options(void)
{
    data_hub_channel_info_t channels[DATA_HUB_MAX_CHANNELS];
    size_t n = data_hub_list_channels(channels, DATA_HUB_MAX_CHANNELS);

    if (n == 0) {
        strncpy(s_channel_names[0], "--", sizeof(s_channel_names[0]) - 1);
        s_channel_names[0][sizeof(s_channel_names[0]) - 1] = '\0';
        s_channel_count = 1;
        snprintf(s_channel_options, sizeof(s_channel_options), "--");
        return;
    }

    s_channel_count = n;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        strncpy(s_channel_names[i], channels[i].name, sizeof(s_channel_names[i]) - 1);
        s_channel_names[i][sizeof(s_channel_names[i]) - 1] = '\0';

        int len = snprintf(s_channel_options + off, sizeof(s_channel_options) - off,
                            "%s%s", (i == 0) ? "" : "\n", s_channel_names[i]);
        if (len < 0 || (size_t)len >= sizeof(s_channel_options) - off) {
            break;
        }
        off += (size_t)len;
    }
}

static uint32_t graph_config_find_channel_index(void)
{
    for (size_t i = 0; i < s_channel_count; i++) {
        if (strcmp(s_channel_names[i], s_selected_channel) == 0) {
            return (uint32_t)i;
        }
    }
    return 0;
}

static void graph_config_update_effective_label(void)
{
    uint32_t tick_s = s_tick_seconds[lv_roller_get_selected(s_tick_roller)];
    uint32_t span_s = s_span_seconds[lv_roller_get_selected(s_span_roller)];

    uint32_t points;
    uint32_t eff_span_s;
    bool capped;
    graph_compute_effective(tick_s, span_s, &points, &eff_span_s, &capped);

    char span_buf[16];
    format_duration(eff_span_s, span_buf, sizeof(span_buf));

    char buf[80];
    if (capped) {
        snprintf(buf, sizeof(buf), "%u pts, capped to last %s (RAM limit)", (unsigned)points, span_buf);
    } else {
        snprintf(buf, sizeof(buf), "%u pts over %s", (unsigned)points, span_buf);
    }
    lv_label_set_text(s_config_effective_label, buf);
}

static void graph_config_roller_changed_cb(lv_event_t *e)
{
    graph_config_update_effective_label();
}

static void graph_config_back_cb(lv_event_t *e)
{
    screen_pop();
}

static void graph_config_apply_cb(lv_event_t *e)
{
    uint32_t ch_idx = lv_roller_get_selected(s_channel_roller);
    if (ch_idx < s_channel_count) {
        strncpy(s_selected_channel, s_channel_names[ch_idx], sizeof(s_selected_channel) - 1);
        s_selected_channel[sizeof(s_selected_channel) - 1] = '\0';
    }
    s_tick_idx = lv_roller_get_selected(s_tick_roller);
    s_span_idx = lv_roller_get_selected(s_span_roller);

    screen_pop(); // back to "graph", which rebuilds fresh with this selection
}

void graph_config_screen_create(void)
{
    graph_config_build_channel_options();

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
    lv_obj_add_event_cb(btn_back, graph_config_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Graph Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_gap(body, 8, 0);

    lv_obj_t *rollers_row = lv_obj_create(body);
    lv_obj_remove_style_all(rollers_row);
    lv_obj_set_size(rollers_row, lv_pct(100), 70);
    lv_obj_set_flex_flow(rollers_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(rollers_row, 4, 0);

    s_channel_roller = lv_roller_create(rollers_row);
    lv_roller_set_options(s_channel_roller, s_channel_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_flex_grow(s_channel_roller, 1);
    lv_obj_set_height(s_channel_roller, 70);
    lv_roller_set_selected(s_channel_roller, graph_config_find_channel_index(), LV_ANIM_OFF);

    s_tick_roller = lv_roller_create(rollers_row);
    lv_roller_set_options(s_tick_roller, s_tick_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_flex_grow(s_tick_roller, 1);
    lv_obj_set_height(s_tick_roller, 70);
    lv_roller_set_selected(s_tick_roller, s_tick_idx, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_tick_roller, graph_config_roller_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_span_roller = lv_roller_create(rollers_row);
    lv_roller_set_options(s_span_roller, s_span_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_flex_grow(s_span_roller, 1);
    lv_obj_set_height(s_span_roller, 70);
    lv_roller_set_selected(s_span_roller, s_span_idx, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_span_roller, graph_config_roller_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_config_effective_label = lv_label_create(body);
    lv_obj_set_style_text_color(s_config_effective_label, lv_color_hex(0x888888), 0);

    lv_obj_t *btn_apply = lv_button_create(body);
    lv_obj_set_size(btn_apply, lv_pct(100), 40);
    lv_obj_add_event_cb(btn_apply, graph_config_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_apply = lv_label_create(btn_apply);
    lv_label_set_text(lbl_apply, "Apply");
    lv_obj_center(lbl_apply);

    lvgl_port_unlock();

    graph_config_update_effective_label();
}
