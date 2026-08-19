#include <stdbool.h>
#include <stdint.h>

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
#include "ui.h"

static const char *TAG = "main";

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

    // Shown as early as possible, before any of the slower/fallible
    // hardware bring-up below (UART handshake, SD mount, WiFi negotiation
    // — the latter alone can take up to 20s falling back to the setup
    // AP). data_hub_init() above is this call's only hard prerequisite —
    // every other subsystem the screens read from (uart_link, sd_storage,
    // logger, web_server) is polled through their own refresh timers via
    // plain static-default reads (mcu_present=false, mounted=false,
    // running=false, wall clock unset) that are safe to see before that
    // subsystem's own _init() has even run, so the tiles/settings/etc.
    // screens just show "not there yet" until each one catches up.
    ui_init();

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

    // One-shot boot self-test: each channel individually, so a dead LED
    // or wiring fault is visible once at boot. The loop below takes over
    // afterward as a steady status indicator — the LED's job settled
    // between "decorative smoke test" and "means something", not both
    // running forever (see CLAUDE.md's rgb_led entry).
    rgb_led_set_rgb(0xFF, 0x00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(400));
    rgb_led_set_rgb(0x00, 0xFF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(400));
    rgb_led_set_rgb(0x00, 0x00, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(400));
    rgb_led_set_rgb(0x00, 0x00, 0x00);

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

    // Status indicator: green once both the companion MCU and the SD
    // card are present, red if neither is, yellow (both channels on) for
    // one but not the other. WiFi isn't part of this — that's already
    // visible on the settings screen, and a 3-color LED runs out of
    // clean states fast once you're trying to encode more than two
    // independent yes/no signals in it.
    uint32_t loop_count = 0;
    while (1)
    {
        bool mcu_ok = uart_link_mcu_present();
        bool sd_ok = sd_storage_is_mounted();

        if (mcu_ok && sd_ok) {
            rgb_led_set_rgb(0x00, 0xFF, 0x00);
        } else if (mcu_ok || sd_ok) {
            rgb_led_set_rgb(0xFF, 0xFF, 0x00);
        } else {
            rgb_led_set_rgb(0xFF, 0x00, 0x00);
        }

        if (++loop_count % 20 == 0) {
            dump_data_hub_channels();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
