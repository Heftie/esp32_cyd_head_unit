#include "tiles_screen.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "data_hub.h"
#include "measurement_screen.h"
#include "screen_nav.h"
#include "web_server.h"

typedef struct {
    char name[DATA_HUB_NAME_LEN];
    lv_obj_t *label_value;
} channel_tile_t;

static channel_tile_t s_tiles[DATA_HUB_MAX_CHANNELS];
static size_t s_tile_count;
static lv_obj_t *s_tile_container;
static lv_obj_t *s_empty_label;
static lv_obj_t *s_clock_label;
static lv_timer_t *s_tile_refresh_timer;
static bool s_tile_screen_active;

static void tile_clicked_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    measurement_screen_set_channel(name);
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

    // DD/MM/YYYY HH:MM:SS — always this format, not locale-dependent,
    // per how this device's owner reads dates. UTC, same as every other
    // timestamp in this firmware (see set_time_screen.c/web_server.c).
    // Sized well above what the format string can ever actually need
    // (max 19 chars + NUL) — GCC's -Wformat-truncation can't tell
    // tm_year/tm_mday/etc. are bounded and assumes worst-case %d width
    // for a 24-byte buffer, which trips -Werror.
    char clock_buf[80];
    time_t now;
    if (web_server_get_wall_clock(&now)) {
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        snprintf(clock_buf, sizeof(clock_buf), "%02d/%02d/%04d %02d:%02d:%02d",
                 tm_utc.tm_mday, tm_utc.tm_mon + 1, tm_utc.tm_year + 1900,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    } else {
        snprintf(clock_buf, sizeof(clock_buf), "Time not set");
    }

    lvgl_port_lock(0);

    lv_label_set_text(s_clock_label, clock_buf);

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

// Rebuilds from scratch each time it's shown (matching the rest of this
// component's single-active-screen pattern) — tile state is re-populated
// from data_hub on the next refresh tick rather than carried across
// screen switches.
void tiles_screen_create(void)
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

    lv_obj_t *clock_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(clock_bar);
    lv_obj_set_size(clock_bar, lv_pct(100), 20);
    lv_obj_set_style_pad_hor(clock_bar, 8, 0);

    s_clock_label = lv_label_create(clock_bar);
    lv_label_set_text(s_clock_label, "Time not set");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x888888), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_LEFT_MID, 0, 0);

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
