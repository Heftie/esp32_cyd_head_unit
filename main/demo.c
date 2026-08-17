#include <stdio.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_check.h>

#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "lcd.h"
#include "touch.h"
#include "hardware.h"
#include "data_hub.h"
#include "uart_link.h"
#include "rgb_led.h"

static const char *TAG="demo";

lv_obj_t *lbl_counter;


typedef struct {
    lv_obj_t *label_title;
    lv_obj_t *label_value;
    lv_obj_t *label_unit;
} multimeter_ui_t;

multimeter_ui_t ui;

void multimeter_create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Titel
    ui.label_title = lv_label_create(scr);
    lv_label_set_text(ui.label_title, "DC Voltage");
    lv_obj_set_style_text_color(ui.label_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui.label_title, &lv_font_montserrat_20, 0);
    lv_obj_align(ui.label_title, LV_ALIGN_TOP_MID, 0, 10);

    // Wert
    ui.label_value = lv_label_create(scr);
    lv_label_set_text(ui.label_value, "0.00");
    lv_obj_set_style_text_color(ui.label_value, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(ui.label_value, &lv_font_montserrat_28, 0);
    lv_obj_align(ui.label_value, LV_ALIGN_CENTER, -20, 20);

    // Einheit
    ui.label_unit = lv_label_create(scr);
    lv_label_set_text(ui.label_unit, "V");
    lv_obj_set_style_text_color(ui.label_unit, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui.label_unit, &lv_font_montserrat_28, 0);
    lv_obj_align_to(ui.label_unit, ui.label_value, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
}

void multimeter_update(float value, const char *unit)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", value);

    lv_label_set_text(ui.label_value, buf);
    lv_label_set_text(ui.label_unit, unit);

    // Startfarbe = grün
    lv_color_t color = lv_color_hex(0x00FF00);

    if (value > 10.0) {
        color = lv_color_hex(0xFFFF00); // gelb
    }

    if (value > 20.0) {
        color = lv_color_hex(0xFF0000); // rot
    }

    lv_obj_set_style_text_color(ui.label_value, color, 0);
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

void touch_test_create_ui(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Ganzflächiger Bereich, der Touch-Punkte als Punkte anzeigt
    touch_test_ui.area = lv_obj_create(scr);
    lv_obj_remove_style_all(touch_test_ui.area);
    lv_obj_set_size(touch_test_ui.area, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(touch_test_ui.area, 0, 0);
    lv_obj_add_flag(touch_test_ui.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_test_ui.area, touch_test_draw_cb, LV_EVENT_PRESSING, NULL);

    // Anzeige der aktuellen Touch-Koordinaten
    touch_test_ui.label = lv_label_create(scr);
    lv_label_set_text(touch_test_ui.label, "Bildschirm beruehren zum Testen");
    lv_obj_set_style_text_color(touch_test_ui.label, lv_color_white(), 0);
    lv_obj_align(touch_test_ui.label, LV_ALIGN_TOP_MID, 0, 5);

    // Button zum Loeschen der Punkte
    lv_obj_t *btn_clear = lv_button_create(scr);
    lv_obj_set_size(btn_clear, 80, 35);
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(btn_clear, touch_test_clear_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, "Clear");
    lv_obj_center(lbl_clear);

    lvgl_port_unlock();
}

void ui_event_Screen(lv_event_t *e)
{
static uint8_t pos=1;

    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_user_data(e);

    if (event_code == LV_EVENT_CLICKED)
    {
        lv_obj_align(btn, pos++, 0, 0);
        if (pos > 9) pos=1;
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

static esp_err_t app_lvgl_main(void)
{
    lv_obj_t *scr = lv_scr_act();

    lvgl_port_lock(0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello LVGL 9 and esp_lvgl_port!");
    lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -48);

    lv_obj_t *labelR = lv_label_create(scr);
    lv_label_set_text(labelR, "Red");
    lv_obj_set_style_text_color(labelR, lv_color_make(0xff, 0, 0), LV_STATE_DEFAULT);
    lv_obj_align(labelR, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *labelG = lv_label_create(scr);
    lv_label_set_text(labelG, "Green");
    lv_obj_set_style_text_color(labelG, lv_color_make(0, 0xff, 0), LV_STATE_DEFAULT);
    lv_obj_align(labelG, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *labelB = lv_label_create(scr);
    lv_label_set_text(labelB, "Blue");
    lv_obj_set_style_text_color(labelB, lv_color_make(0, 0, 0xff), LV_STATE_DEFAULT);
    lv_obj_align(labelB, LV_ALIGN_TOP_MID, 0, 64);

    lv_obj_t *btn_counter = lv_button_create(scr);
    lv_obj_align(btn_counter, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(btn_counter, 120, 50);
    lv_obj_add_event_cb(btn_counter, ui_event_Screen, LV_EVENT_ALL, btn_counter);

    lbl_counter = lv_label_create(btn_counter);
    lv_label_set_text(lbl_counter, "testing");
    lv_obj_set_style_text_color(lbl_counter, lv_color_make(248, 11, 181), LV_STATE_DEFAULT);
    lv_obj_align(lbl_counter, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();

    return ESP_OK;
}


void app_main(void)
{
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t lcd_panel;
    esp_lcd_touch_handle_t tp;
    lvgl_port_touch_cfg_t touch_cfg;
    lv_display_t *lvgl_display = NULL;
    char buf[16];
    uint16_t n = 0;

    ESP_ERROR_CHECK(lcd_display_brightness_init());

    ESP_ERROR_CHECK(app_lcd_init(&lcd_io, &lcd_panel));
    lvgl_display = app_lvgl_init(lcd_io, lcd_panel);
    if (lvgl_display == NULL)
    {
        ESP_LOGI(TAG, "fatal error in app_lvgl_init");
        esp_restart();
    }
    
    ESP_ERROR_CHECK(touch_init(&tp));
    touch_cfg.disp = lvgl_display;
    touch_cfg.handle = tp;
    touch_cfg.scale.x = 0;
    touch_cfg.scale.y = 0;
    lvgl_port_add_touch(&touch_cfg);

    ESP_ERROR_CHECK(lcd_display_brightness_set(75));
    ESP_ERROR_CHECK(lcd_display_rotate(lvgl_display, LV_DISPLAY_ROTATION_90));
    //ESP_ERROR_CHECK(app_lvgl_main());

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

    touch_test_create_ui();
    //multimeter_create_ui();
     float v = 0.0;
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
        if(led_counter > 40) led_counter = 0;

        v += 0.5;
        if (v > 25) v = 0.0;

        //multimeter_update(v, "V");

        if (++loop_count % 20 == 0) {
            dump_data_hub_channels();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelay(portMAX_DELAY);
}
