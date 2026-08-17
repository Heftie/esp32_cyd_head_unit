#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>

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

static void tile_ui_create(void);

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
    tile_ui_create();
}

// Diagnostics screen — verifies the panel after any hardware change.
// Reachable from the dashboard's Settings button; Back returns there.
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
    touch_test_create_ui();
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
    lv_obj_set_size(btn_settings, 70, 26);
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

    tile_ui_create();

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
