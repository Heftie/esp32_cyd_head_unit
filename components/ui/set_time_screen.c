#include "set_time_screen.h"

#include <stdio.h>
#include <time.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "screen_nav.h"
#include "web_server.h"

// Fixed, hand-written roller option lists rather than generated at
// runtime — these never change, so there's nothing to compute. Day
// doesn't adjust for the selected month's actual length (e.g. Feb 30 is
// selectable); accepted for a manual-fallback picker that only exists
// because SNTP isn't reachable, not worth the extra roller wiring.
static const char *s_year_options =
    "2024\n2025\n2026\n2027\n2028\n2029\n2030\n2031\n2032\n2033\n2034\n2035";
static const char *s_month_options =
    "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12";
static const char *s_day_options =
    "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n"
    "21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31";
static const char *s_hour_options =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
static const char *s_minute_options =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n"
    "21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n"
    "41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

#define YEAR_OPTIONS_BASE 2024

static lv_obj_t *s_set_time_status_label;
static lv_obj_t *s_year_roller;
static lv_obj_t *s_month_roller;
static lv_obj_t *s_day_roller;
static lv_obj_t *s_hour_roller;
static lv_obj_t *s_minute_roller;

static void set_time_back_cb(lv_event_t *e)
{
    screen_pop();
}

// Days since 1970-01-01 for a proleptic Gregorian civil date — Howard
// Hinnant's well-known constant-time algorithm. Used instead of libc's
// timegm()/mktime() so this doesn't depend on a timezone-aware libc
// build; every timestamp in this firmware (SNTP, logger, web_server) is
// already UTC-only with no timezone concept, so this matches that.
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);            // [0, 399]
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

static void set_time_apply_cb(lv_event_t *e)
{
    int year = YEAR_OPTIONS_BASE + (int)lv_roller_get_selected(s_year_roller);
    int month = 1 + (int)lv_roller_get_selected(s_month_roller);
    int day = 1 + (int)lv_roller_get_selected(s_day_roller);
    int hour = (int)lv_roller_get_selected(s_hour_roller);
    int minute = (int)lv_roller_get_selected(s_minute_roller);

    int64_t days = days_from_civil(year, month, day);
    time_t epoch_utc = (time_t)(days * 86400 + hour * 3600 + minute * 60);

    web_server_set_wall_clock(epoch_utc);

    lv_label_set_text(s_set_time_status_label, "Time set.");
}

static lv_obj_t *set_time_create_roller(lv_obj_t *parent, const char *options, uint32_t selected)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_flex_grow(roller, 1);
    lv_obj_set_height(roller, 70);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    return roller;
}

void set_time_screen_create(void)
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
    lv_obj_add_event_cb(btn_back, set_time_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Set Time (UTC)");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height (240 screen - 34 header) and SCROLLABLE
    // cleared — see measurement_screen.c's note on why this matters more
    // than it looks like it should.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_gap(body, 8, 0);

    s_set_time_status_label = lv_label_create(body);
    lv_label_set_text(s_set_time_status_label, "Y / M / D / h / m, then Set.");
    lv_obj_set_style_text_color(s_set_time_status_label, lv_color_hex(0xaaaaaa), 0);

    lv_obj_t *rollers_row = lv_obj_create(body);
    lv_obj_remove_style_all(rollers_row);
    lv_obj_set_size(rollers_row, lv_pct(100), 70);
    lv_obj_set_flex_flow(rollers_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(rollers_row, 4, 0);

    // Default selection is arbitrary (this device has no clock to guess
    // "now" from, which is the entire reason this screen exists) — first
    // option on every roller.
    s_year_roller = set_time_create_roller(rollers_row, s_year_options, 0);
    s_month_roller = set_time_create_roller(rollers_row, s_month_options, 0);
    s_day_roller = set_time_create_roller(rollers_row, s_day_options, 0);
    s_hour_roller = set_time_create_roller(rollers_row, s_hour_options, 0);
    s_minute_roller = set_time_create_roller(rollers_row, s_minute_options, 0);

    lv_obj_t *btn_apply = lv_button_create(body);
    lv_obj_set_size(btn_apply, lv_pct(100), 40);
    lv_obj_add_event_cb(btn_apply, set_time_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_apply = lv_label_create(btn_apply);
    lv_label_set_text(lbl_apply, "Set time");
    lv_obj_center(lbl_apply);

    lvgl_port_unlock();
}
