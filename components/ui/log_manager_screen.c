#include "log_manager_screen.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "logger.h"
#include "screen_nav.h"
#include "sd_storage.h"

#define LOG_MANAGER_MAX_FILES 16

// Per-row filename storage — sd_storage_list_dir() fills a stack array
// each refresh, but each row's Delete button needs a stable pointer to
// its own filename for its event callback to read back later, well
// after the stack array that produced it is gone.
static char s_file_names[LOG_MANAGER_MAX_FILES][SD_STORAGE_NAME_LEN];

static lv_obj_t *s_name_ta;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_status_label;
static lv_obj_t *s_file_list;
static lv_obj_t *s_clear_label;
static lv_timer_t *s_refresh_timer;
static bool s_screen_active;

// Which file (if any) has one armed Delete tap pending its confirming
// second tap — at most one at a time, matching settings' Forget-network
// pattern but scoped to a filename instead of a single global action.
static char s_delete_armed_name[SD_STORAGE_NAME_LEN];
static lv_timer_t *s_delete_disarm_timer;
static bool s_clear_armed;
static lv_timer_t *s_clear_disarm_timer;
static volatile bool s_clear_in_progress;

static void log_manager_refresh_files(void);

static void log_manager_back_cb(lv_event_t *e)
{
    s_screen_active = false;
    screen_pop();
}

static void log_manager_ta_focus_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(s_keyboard, s_name_ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void log_manager_kb_hide_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Applying a name both selects it and starts logging under it right
// away — there's no separate "just remember this for later" state to
// keep track of. logger_start() no-ops the rename if it's unchanged, so
// re-pressing "Use" on the current name just resumes if it was stopped.
static void log_manager_use_name_cb(lv_event_t *e)
{
    const char *name = lv_textarea_get_text(s_name_ta);
    logger_start((name != NULL && name[0] != '\0') ? name : NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    log_manager_refresh_files();
}

static void log_manager_delete_disarm_cb(lv_timer_t *timer)
{
    s_delete_armed_name[0] = '\0';
    s_delete_disarm_timer = NULL;
    log_manager_refresh_files();
}

static void log_manager_delete_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);

    if (strcmp(s_delete_armed_name, name) != 0) {
        strncpy(s_delete_armed_name, name, sizeof(s_delete_armed_name) - 1);
        s_delete_armed_name[sizeof(s_delete_armed_name) - 1] = '\0';
        if (s_delete_disarm_timer != NULL) {
            lv_timer_del(s_delete_disarm_timer);
        }
        s_delete_disarm_timer = lv_timer_create(log_manager_delete_disarm_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_delete_disarm_timer, 1);
        log_manager_refresh_files(); // redraw so this row shows the confirm state
        return;
    }

    if (s_delete_disarm_timer != NULL) {
        lv_timer_del(s_delete_disarm_timer);
        s_delete_disarm_timer = NULL;
    }
    sd_storage_erase(name);
    s_delete_armed_name[0] = '\0';
    log_manager_refresh_files();
}

static void log_manager_clear_disarm_cb(lv_timer_t *timer)
{
    s_clear_armed = false;
    s_clear_disarm_timer = NULL;
    lv_label_set_text(s_clear_label, "Clear SD card");
}

// sd_storage_format() on a real card can take several seconds — running it
// straight from the click callback would block the whole esp_lvgl_port task
// (redraw + touch input both stall) until it returns, which froze the
// screen right at "Tap again to confirm" the one time this was tried on
// real hardware, since the "Clearing..." label update never got a chance
// to actually paint before the block. Runs on its own task instead, and
// only touches LVGL objects through lvgl_port_lock()/unlock() since it's
// no longer running on the LVGL task itself.
static void log_manager_clear_task(void *arg)
{
    logger_stop();
    sd_storage_format();

    // s_screen_active is written from the LVGL task (log_manager_back_cb)
    // and read here from this task without a lock — same informal
    // convention web_server.c uses for its own cross-task status fields
    // (see its comment on boot_epoch_offset_us). Worst case is one stale
    // read; skipping the UI touch is always safe, unlike writing into
    // whatever screen's now active if the user already left this one.
    if (s_screen_active) {
        lvgl_port_lock(0);
        lv_label_set_text(s_clear_label, "Clear SD card");
        lvgl_port_unlock();
        log_manager_refresh_files();
    }

    s_clear_in_progress = false;
    vTaskDelete(NULL);
}

// Stops logging first — sd_storage_format() wipes the card out from
// under whatever file logger thinks it's still appending to, and this
// way logger_is_running() correctly reads false afterward (matching
// reality: there's nothing left to log to) instead of silently
// recreating a headerless file on the next flush.
static void log_manager_clear_cb(lv_event_t *e)
{
    if (s_clear_in_progress) {
        return;
    }

    if (!s_clear_armed) {
        s_clear_armed = true;
        lv_label_set_text(s_clear_label, "Tap again to confirm");
        s_clear_disarm_timer = lv_timer_create(log_manager_clear_disarm_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_clear_disarm_timer, 1);
        return;
    }

    if (s_clear_disarm_timer != NULL) {
        lv_timer_del(s_clear_disarm_timer);
        s_clear_disarm_timer = NULL;
    }
    lv_label_set_text(s_clear_label, "Clearing...");
    s_clear_armed = false;
    s_clear_in_progress = true;
    xTaskCreate(log_manager_clear_task, "log_clear", 3072, NULL, 4, NULL);
}

static void log_manager_refresh_files(void)
{
    sd_storage_dir_entry_t entries[LOG_MANAGER_MAX_FILES];
    size_t n = sd_storage_list_dir(NULL, entries, LOG_MANAGER_MAX_FILES);
    const char *active_path = logger_get_current_path();
    bool logging = logger_is_running();

    lvgl_port_lock(0);

    char status_buf[80];
    snprintf(status_buf, sizeof(status_buf), "Logging to: %s (%s)",
             active_path[0] ? active_path : "--", logging ? "running" : "stopped");
    lv_label_set_text(s_status_label, status_buf);

    lv_obj_clean(s_file_list);
    for (size_t i = 0; i < n; i++) {
        strncpy(s_file_names[i], entries[i].name, sizeof(s_file_names[i]) - 1);
        s_file_names[i][sizeof(s_file_names[i]) - 1] = '\0';

        lv_obj_t *row = lv_obj_create(s_file_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 24);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(row);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s (%u KB)", s_file_names[i], (unsigned)(entries[i].size_bytes / 1024));
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);

        // The file logger is actively writing to isn't offered for
        // deletion — sd_storage_erase()-ing it out from under an open
        // logging session is a footgun with no upside over just
        // stopping first.
        if (strcmp(s_file_names[i], active_path) == 0 && logging) {
            lv_obj_t *tag = lv_label_create(row);
            lv_label_set_text(tag, "active");
            lv_obj_set_style_text_color(tag, lv_color_hex(0x7FBD8A), 0);
            continue;
        }

        lv_obj_t *btn_del = lv_button_create(row);
        lv_obj_set_size(btn_del, 76, 22);
        lv_obj_add_event_cb(btn_del, log_manager_delete_cb, LV_EVENT_CLICKED, s_file_names[i]);
        lv_obj_t *lbl_del = lv_label_create(btn_del);
        lv_label_set_text(lbl_del, strcmp(s_file_names[i], s_delete_armed_name) == 0 ? "Confirm?" : "Delete");
        lv_obj_center(lbl_del);
    }

    lvgl_port_unlock();
}

static void log_manager_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_screen_active) {
        return;
    }
    log_manager_refresh_files();
}

void log_manager_screen_create(void)
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
    lv_obj_add_event_cb(btn_back, log_manager_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Manage Logs");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Explicit body height and SCROLLABLE cleared on body itself — see
    // measurement_screen.c's note on why. s_file_list keeps its own
    // scrolling on, deliberately: that one's an actual list that can
    // outgrow its space, not a layout that should never need to.
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), 206);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_gap(body, 6, 0);

    lv_obj_t *name_row = lv_obj_create(body);
    lv_obj_remove_style_all(name_row);
    lv_obj_set_size(name_row, lv_pct(100), 32);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(name_row, 6, 0);

    s_name_ta = lv_textarea_create(name_row);
    lv_obj_set_height(s_name_ta, 32);
    lv_obj_set_flex_grow(s_name_ta, 1);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_placeholder_text(s_name_ta, "log.csv");
    lv_textarea_set_max_length(s_name_ta, SD_STORAGE_NAME_LEN - 1);
    lv_obj_add_event_cb(s_name_ta, log_manager_ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *btn_use = lv_button_create(name_row);
    lv_obj_set_size(btn_use, 60, 32);
    lv_obj_add_event_cb(btn_use, log_manager_use_name_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_use = lv_label_create(btn_use);
    lv_label_set_text(lbl_use, "Use");
    lv_obj_center(lbl_use);

    s_status_label = lv_label_create(body);
    lv_label_set_text(s_status_label, "Loading...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x888888), 0);

    s_file_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_file_list);
    lv_obj_set_width(s_file_list, lv_pct(100));
    lv_obj_set_flex_grow(s_file_list, 1);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_file_list, 4, 0);

    lv_obj_t *btn_clear = lv_button_create(body);
    lv_obj_set_size(btn_clear, lv_pct(100), 32);
    lv_obj_add_event_cb(btn_clear, log_manager_clear_cb, LV_EVENT_CLICKED, NULL);
    s_clear_label = lv_label_create(btn_clear);
    lv_label_set_text(s_clear_label, "Clear SD card");
    lv_obj_center(s_clear_label);

    // Overlay, not a body child — sits on top of the file list rather
    // than reflowing it, and only needs showing/hiding on textarea
    // focus, not its own space in body's budget.
    s_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_keyboard, lv_pct(100), 140);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_name_ta);
    lv_obj_add_event_cb(s_keyboard, log_manager_kb_hide_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, log_manager_kb_hide_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    s_delete_armed_name[0] = '\0';
    s_clear_armed = false;
    s_screen_active = true;

    lvgl_port_unlock();

    if (s_refresh_timer == NULL) {
        s_refresh_timer = lv_timer_create(log_manager_refresh_timer_cb, 2000, NULL);
    }
    log_manager_refresh_files();
}
