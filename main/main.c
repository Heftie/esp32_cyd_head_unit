#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_random.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "lcd.h"
#include "touch.h"
#include "hardware.h"
#include "data_hub.h"
#include "uart_link.h"
#include "rgb_led.h"
#include "sd_storage.h"
#include "logger.h"
#include "web_server.h"

static const char *TAG = "main";

// --- Screen navigation: a small registry + back stack, so a screen enters
// another one by name (screen_push()) instead of calling its constructor
// directly. Only "tiles" and "touch_test" are registered today, but this
// is the piece any future screen (settings, wifi status, measurement)
// builds on rather than each one hand-wiring its own forward/back call.

typedef void (*screen_create_fn_t)(void);

typedef struct {
    const char *name;
    screen_create_fn_t create;
} screen_entry_t;

#define SCREEN_REGISTRY_MAX 8
#define SCREEN_STACK_MAX    8
#define SCREEN_HOME         "tiles"

static screen_entry_t s_screen_registry[SCREEN_REGISTRY_MAX];
static size_t s_screen_registry_count;
static const char *s_screen_stack[SCREEN_STACK_MAX];
static size_t s_screen_stack_depth;
static const char *s_current_screen;

static void screen_register(const char *name, screen_create_fn_t create)
{
    if (s_screen_registry_count >= SCREEN_REGISTRY_MAX) {
        ESP_LOGE(TAG, "screen_register: registry full, dropping \"%s\"", name);
        return;
    }
    s_screen_registry[s_screen_registry_count].name = name;
    s_screen_registry[s_screen_registry_count].create = create;
    s_screen_registry_count++;
}

static screen_create_fn_t screen_lookup(const char *name)
{
    for (size_t i = 0; i < s_screen_registry_count; i++) {
        if (strcmp(s_screen_registry[i].name, name) == 0) {
            return s_screen_registry[i].create;
        }
    }
    return NULL;
}

static void screen_activate(const char *name)
{
    screen_create_fn_t create = screen_lookup(name);
    if (create == NULL) {
        ESP_LOGE(TAG, "screen_activate: unknown screen \"%s\"", name);
        return;
    }
    s_current_screen = name;
    create();
}

// Enters `name`, remembering the screen it was called from so screen_pop()
// can return here. Use this from a screen that leads somewhere new (e.g. a
// Settings button) instead of calling the target screen's create function
// directly.
static void screen_push(const char *name)
{
    if (s_current_screen != NULL) {
        if (s_screen_stack_depth < SCREEN_STACK_MAX) {
            s_screen_stack[s_screen_stack_depth++] = s_current_screen;
        } else {
            ESP_LOGW(TAG, "screen_push: back stack full, losing \"%s\"", s_current_screen);
        }
    }
    screen_activate(name);
}

// Returns to whichever screen called screen_push() to get here. Falls back
// to the home screen if the stack is empty — e.g. Back pressed on a screen
// that was entered via screen_activate() directly rather than
// screen_push() (only app_main's initial screen does that today).
static void screen_pop(void)
{
    if (s_screen_stack_depth > 0) {
        screen_activate(s_screen_stack[--s_screen_stack_depth]);
    } else {
        screen_activate(SCREEN_HOME);
    }
}

typedef struct {
    lv_obj_t *area;
    lv_obj_t *label;
} touch_test_ui_t;

touch_test_ui_t touch_test_ui;

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

// Diagnostics screen — verifies the panel after any hardware change.
// Reachable from the settings screen; Back (screen_pop()) returns to
// whichever screen pushed it here.
static void touch_test_create_ui(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    // lv_obj_clean() only removes children — the screen object itself is
    // reused across screens, so tile_ui_create()'s flex layout/padding
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

// --- Settings: WiFi status (mode, SSID, IP, time-sync) via web_server's
// status snapshot, a "forget this network" action, and a way into the
// touch diagnostics screen — reached from the dashboard's Settings button
// instead of touch_test directly, so touch_test's own Back button (which
// just calls screen_pop(), see above) now returns here.

static lv_obj_t *s_settings_status_label;
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

static void settings_diagnostics_cb(lv_event_t *e)
{
    s_settings_screen_active = false;
    screen_push("touch_test");
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

    char buf[160];
    snprintf(buf, sizeof(buf), "WiFi: %s\nSSID: %s\nIP: %s\nTime: %s",
             mode_str,
             status.ssid[0] ? status.ssid : "--",
             status.ip[0] ? status.ip : "--",
             status.time_synced ? "synced" : "not synced");

    lvgl_port_lock(0);

    lv_label_set_text(s_settings_status_label, buf);

    // Nothing to forget in AP mode — there are no stored credentials yet.
    bool can_forget = (status.wifi_mode == WEB_SERVER_WIFI_STA);
    if (can_forget) {
        lv_obj_add_flag(s_settings_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_forget_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(s_settings_forget_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_settings_forget_btn, LV_OPA_50, 0);
    }

    lvgl_port_unlock();
}

static void settings_create_ui(void)
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

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 14, 0);

    s_settings_status_label = lv_label_create(body);
    lv_label_set_text(s_settings_status_label, "Loading status...");
    lv_obj_set_style_text_color(s_settings_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(s_settings_status_label, 4, 0);

    s_settings_forget_btn = lv_button_create(body);
    lv_obj_set_size(s_settings_forget_btn, 180, 40);
    lv_obj_add_event_cb(s_settings_forget_btn, settings_forget_cb, LV_EVENT_CLICKED, NULL);
    s_settings_forget_label = lv_label_create(s_settings_forget_btn);
    lv_label_set_text(s_settings_forget_label, "Forget network");
    lv_obj_center(s_settings_forget_label);

    lv_obj_t *btn_diag = lv_button_create(body);
    lv_obj_set_size(btn_diag, 180, 40);
    lv_obj_add_event_cb(btn_diag, settings_diagnostics_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_diag = lv_label_create(btn_diag);
    lv_label_set_text(lbl_diag, "Touch diagnostics");
    lv_obj_center(lbl_diag);

    s_settings_forget_armed = false;
    s_settings_screen_active = true;

    lvgl_port_unlock();

    if (s_settings_refresh_timer == NULL) {
        s_settings_refresh_timer = lv_timer_create(settings_refresh_timer_cb, 1000, NULL);
    }
}

// --- Dashboard: one tile per data_hub channel, created on first sight and
// refreshed on a timer. Nothing here assumes a fixed set of channels — the
// same screen works whether the companion MCU exposes one reading or ten.

typedef struct {
    char name[DATA_HUB_NAME_LEN];
    lv_obj_t *label_value;
} channel_tile_t;

static channel_tile_t s_tiles[DATA_HUB_MAX_CHANNELS];
static size_t s_tile_count;
static lv_obj_t *s_tile_container;
static lv_obj_t *s_empty_label;
static lv_timer_t *s_tile_refresh_timer;
static bool s_tile_screen_active;

// Which channel the measurement screen (below) should show — set by
// tile_clicked_cb() just before pushing there, since screen_push() only
// takes a name and this file has exactly one instance of each screen.
static char s_measurement_channel[DATA_HUB_NAME_LEN];

static void tile_clicked_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    strncpy(s_measurement_channel, name, sizeof(s_measurement_channel) - 1);
    s_measurement_channel[sizeof(s_measurement_channel) - 1] = '\0';
    s_tile_screen_active = false;
    screen_push("measurement");
}

static channel_tile_t *find_or_create_tile(const char *name)
{
    for (size_t i = 0; i < s_tile_count; i++) {
        if (strcmp(s_tiles[i].name, name) == 0) {
            return &s_tiles[i];
        }
    }
    if (s_tile_count >= DATA_HUB_MAX_CHANNELS) {
        return NULL;
    }

    channel_tile_t *t = &s_tiles[s_tile_count++];
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';

    lv_obj_t *tile = lv_obj_create(s_tile_container);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 130, 78);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1c1c1c), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_style_pad_all(tile, 8, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, tile_clicked_cb, LV_EVENT_CLICKED, t->name);

    lv_obj_t *label_name = lv_label_create(tile);
    lv_label_set_text(label_name, t->name);
    lv_obj_set_style_text_color(label_name, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(label_name, LV_ALIGN_TOP_LEFT, 0, 0);

    t->label_value = lv_label_create(tile);
    lv_label_set_text(t->label_value, "--");
    lv_obj_set_style_text_color(t->label_value, lv_color_hex(0x00e08a), 0);
    lv_obj_set_style_text_font(t->label_value, &lv_font_montserrat_20, 0);
    lv_obj_align(t->label_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return t;
}

static void tile_ui_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_tile_screen_active) {
        return;
    }

    data_hub_channel_info_t channels[DATA_HUB_MAX_CHANNELS];
    size_t n = data_hub_list_channels(channels, DATA_HUB_MAX_CHANNELS);

    lvgl_port_lock(0);

    if (n > 0) {
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < n; i++) {
        channel_tile_t *t = find_or_create_tile(channels[i].name);
        if (t == NULL) {
            continue; // channel table full — see DATA_HUB_MAX_CHANNELS
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%.2f %s", channels[i].latest_value, channels[i].unit);
        lv_label_set_text(t->label_value, buf);
    }

    lvgl_port_unlock();
}

static void tile_ui_settings_cb(lv_event_t *e)
{
    s_tile_screen_active = false;
    screen_push("settings");
}

// Default screen. Rebuilds from scratch each time it's shown (matching the
// rest of this file's single-active-screen pattern) — tile state is
// re-populated from data_hub on the next refresh tick rather than carried
// across screen switches.
static void tile_ui_create(void)
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

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Telemetry");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *btn_settings = lv_button_create(header);
    lv_obj_set_size(btn_settings, 84, 32);
    lv_obj_add_event_cb(btn_settings, tile_ui_settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, "Settings");
    lv_obj_center(lbl_settings);

    s_tile_container = lv_obj_create(scr);
    lv_obj_remove_style_all(s_tile_container);
    lv_obj_set_width(s_tile_container, lv_pct(100));
    lv_obj_set_flex_grow(s_tile_container, 1);
    lv_obj_set_flex_flow(s_tile_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_tile_container, 6, 0);
    lv_obj_set_style_pad_gap(s_tile_container, 6, 0);

    s_empty_label = lv_label_create(s_tile_container);
    lv_label_set_text(s_empty_label, "Waiting for channels...");
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0x888888), 0);

    s_tile_count = 0;
    s_tile_screen_active = true;

    lvgl_port_unlock();

    if (s_tile_refresh_timer == NULL) {
        s_tile_refresh_timer = lv_timer_create(tile_ui_refresh_timer_cb, 500, NULL);
    }
}

// --- Measurement: multimeter-style detail view for one channel, opened by
// tapping its tile (tile_clicked_cb() above). min/max/avg are accumulated
// here rather than read out of data_hub's ring buffer — that buffer is
// only DATA_HUB_HISTORY_LEN (32) samples deep, deliberately just enough to
// bridge to logger's next flush (see data_hub.h), and keeps overwriting
// itself regardless of this screen's Start/Reset state. Stats always
// start fresh on entry, matching this file's rebuild-from-scratch-on-entry
// convention (see tile_ui_create()) — this screen doesn't try to resume a
// run left going from a previous visit.

typedef struct {
    bool running;
    bool has_sample;
    float min;
    float max;
    double sum;
    uint32_t count;
} measurement_stats_t;

static measurement_stats_t s_measurement_stats;
static lv_obj_t *s_measurement_title_label;
static lv_obj_t *s_measurement_value_label;
static lv_obj_t *s_measurement_min_label;
static lv_obj_t *s_measurement_max_label;
static lv_obj_t *s_measurement_avg_label;
static lv_obj_t *s_measurement_run_label;
static lv_timer_t *s_measurement_refresh_timer;
static bool s_measurement_screen_active;

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
    // half off-screen before (see the layout note in measurement_create_ui).
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

static void measurement_create_ui(void)
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

static void dump_data_hub_channels(void)
{
    data_hub_channel_info_t channels[DATA_HUB_MAX_CHANNELS];
    size_t n = data_hub_list_channels(channels, DATA_HUB_MAX_CHANNELS);

    if (n == 0) {
        ESP_LOGI(TAG, "data_hub: no channels yet");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "data_hub: %s = %.2f %s", channels[i].name, channels[i].latest_value, channels[i].unit);
    }
}

#ifdef SIM_DATA
// Stand-in for the companion MCU: publishes two fake channels on the same
// data_hub_publish() path uart_link would use, so the tile UI, logger, and
// web dashboard all have something to show with no MCU on the other end
// of UART2. data_hub_publish() is mutex-guarded internally, so a second
// publisher task alongside uart_link's is safe — see components/data_hub.
// Gated behind the SIM_DATA compile flag (see top-level CMakeLists.txt);
// never enabled in a build meant to talk to real hardware.
static void sim_data_task(void *arg)
{
    while (1) {
        float voltage = 11.5f + ((float)esp_random() / (float)UINT32_MAX) * 1.5f;   // 11.5-13.0 V
        float temp_c  = 20.0f + ((float)esp_random() / (float)UINT32_MAX) * 25.0f;  // 20-45 C
        data_hub_publish("VOLT:DC", voltage, "V");
        data_hub_publish("TEMP", temp_c, "C");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

void app_main(void)
{
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t lcd_panel;
    esp_lcd_touch_handle_t tp;
    lvgl_port_touch_cfg_t touch_cfg;
    lv_display_t *lvgl_display = NULL;

    const lcd_config_t lcd_cfg = {
        .spi_host = LCD_SPI_HOST,
        .spi_clk_gpio = LCD_SPI_CLK,
        .spi_mosi_gpio = LCD_SPI_MOSI,
        .spi_miso_gpio = LCD_SPI_MISO,
        .dc_gpio = LCD_DC,
        .cs_gpio = LCD_CS,
        .reset_gpio = LCD_RESET,
        .backlight_gpio = LCD_BACKLIGHT,
        .backlight_ledc_channel = LCD_BACKLIGHT_LEDC_CH,
        .pixel_clock_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .bits_per_pixel = LCD_BITS_PIXEL,
        .h_res = LCD_H_RES,
        .v_res = LCD_V_RES,
        .draw_buf_lines = LCD_BUF_LINES,
        .double_buffer = LCD_DOUBLE_BUFFER,
        .mirror_x = LCD_MIRROR_X,
        .mirror_y = LCD_MIRROR_Y,
    };

    const touch_config_t touch_hw_cfg = {
        .spi_clk_gpio = TOUCH_SPI_CLK,
        .spi_mosi_gpio = TOUCH_SPI_MOSI,
        .spi_miso_gpio = TOUCH_SPI_MISO,
        .cs_gpio = TOUCH_CS,
        .rst_gpio = TOUCH_RST,
        .irq_gpio = TOUCH_IRQ,
        .h_res = LCD_H_RES,
        .v_res = LCD_V_RES,
        .mirror_x = TOUCH_MIRROR_X,
        .mirror_y = TOUCH_MIRROR_Y,
        .x_res_min = TOUCH_X_RES_MIN,
        .x_res_max = TOUCH_X_RES_MAX,
        .y_res_min = TOUCH_Y_RES_MIN,
        .y_res_max = TOUCH_Y_RES_MAX,
    };

    ESP_ERROR_CHECK(lcd_display_brightness_init(&lcd_cfg));

    ESP_ERROR_CHECK(app_lcd_init(&lcd_cfg, &lcd_io, &lcd_panel));
    lvgl_display = app_lvgl_init(&lcd_cfg, lcd_io, lcd_panel);
    if (lvgl_display == NULL)
    {
        ESP_LOGI(TAG, "fatal error in app_lvgl_init");
        esp_restart();
    }

    ESP_ERROR_CHECK(touch_init(&touch_hw_cfg, &tp));
    touch_cfg.disp = lvgl_display;
    touch_cfg.handle = tp;
    touch_cfg.scale.x = 0;
    touch_cfg.scale.y = 0;
    lvgl_port_add_touch(&touch_cfg);

    ESP_ERROR_CHECK(lcd_display_brightness_set(75));
    ESP_ERROR_CHECK(lcd_display_rotate(lvgl_display, LV_DISPLAY_ROTATION_90));

    data_hub_init();

    const uart_link_config_t uart_cfg = {
        .uart_num = UART_NUM_2,
        .txd_gpio = UART_LINK_TXD,
        .rxd_gpio = UART_LINK_RXD,
        .baud_rate = UART_LINK_BAUD,
    };
    if (uart_link_init(&uart_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uart_link_init failed");
    }

#ifdef SIM_DATA
    xTaskCreate(sim_data_task, "sim_data", 2048, NULL, 4, NULL);
#endif

    const rgb_led_config_t rgb_led_cfg = {
        .red_gpio = RGB_LED_RED,
        .green_gpio = RGB_LED_GREEN,
        .blue_gpio = RGB_LED_BLUE,
        .active_low = RGB_LED_ACTIVE_LOW,
    };
    if (rgb_led_init(&rgb_led_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "rgb_led_init failed");
    }

    // Touch is bit-banged GPIO now (see components/touch), so SD gets SPI3
    // to itself and can just stay mounted — no teardown/reacquire dance.
    const sd_storage_config_t sd_cfg = {
        .spi_host = SD_SPI_HOST,
        .cs_gpio = SD_CS,
        .bus_already_initialized = false,
        .clk_gpio = SD_SPI_CLK,
        .mosi_gpio = SD_SPI_MOSI,
        .miso_gpio = SD_SPI_MISO,
        .mount_point = SD_MOUNT_POINT,
        .max_open_files = 0,
        .format_if_mount_failed = false,
    };
    if (sd_storage_init(&sd_cfg) == ESP_OK) {
        sd_storage_info_t info;
        if (sd_storage_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "sd_storage: %s, %llu MB total, %llu MB free", info.card_type,
                     (unsigned long long)(info.total_bytes / (1024 * 1024)),
                     (unsigned long long)(info.free_bytes / (1024 * 1024)));
        }

        const logger_config_t logger_cfg = {
            .log_path = "log.csv",
            .flush_interval_ms = 5000,
        };
        if (logger_init(&logger_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "logger_init failed");
        }
    } else {
        ESP_LOGW(TAG, "sd_storage_init failed — no card, or wiring not verified yet");
    }

    // Independent of SD: /api/data still works without a card, /download
    // and /api/history just report "no log file yet" if sd_storage_init
    // above failed.
    const web_server_config_t web_cfg = {
        .http_port = 0, // default 80
    };
    if (web_server_init(&web_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web_server_init failed");
    }

    screen_register("tiles", tile_ui_create);
    screen_register("touch_test", touch_test_create_ui);
    screen_register("settings", settings_create_ui);
    screen_register("measurement", measurement_create_ui);
    screen_activate(SCREEN_HOME);

    uint32_t loop_count = 0;
    uint32_t led_counter = 0;
    while (1)
    {
        led_counter += 1;
        switch (led_counter)
        {
        case 1:
            rgb_led_set_rgb(0xFF, 0x00, 0x00);
            break;
        case 20:
            rgb_led_set_rgb(0x00, 0xFF, 0x00);
            break;
        case 30:
            rgb_led_set_rgb(0x00, 0x00, 0xFF);
            break;

        default:

            break;
        }
        if (led_counter > 40) led_counter = 0;

        if (++loop_count % 20 == 0) {
            dump_data_hub_channels();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
