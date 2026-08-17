#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // SPI bus shared by the panel IO
    spi_host_device_t spi_host;
    gpio_num_t spi_clk_gpio;
    gpio_num_t spi_mosi_gpio;
    gpio_num_t spi_miso_gpio;

    // Panel control lines
    gpio_num_t dc_gpio;
    gpio_num_t cs_gpio;
    gpio_num_t reset_gpio;

    // Backlight, driven via LEDC
    gpio_num_t backlight_gpio;
    ledc_channel_t backlight_ledc_channel;

    // Panel timing/geometry
    int pixel_clock_hz;
    int lcd_cmd_bits;
    int lcd_param_bits;
    int bits_per_pixel;
    int h_res;
    int v_res;
    int draw_buf_lines;
    bool double_buffer;
    bool mirror_x;
    bool mirror_y;
} lcd_config_t;

// Configures the LEDC timer/channel used for backlight brightness control.
esp_err_t lcd_display_brightness_init(const lcd_config_t *config);

// 0-100, clamped.
esp_err_t lcd_display_brightness_set(int brightness_percent);
esp_err_t lcd_display_backlight_off(void);
esp_err_t lcd_display_backlight_on(void);
esp_err_t lcd_display_rotate(lv_display_t *lvgl_disp, lv_display_rotation_t dir);

// Brings up the SPI bus, panel IO and panel driver (ili9341 if CYD_ILI9341
// is defined, st7789 otherwise).
esp_err_t app_lcd_init(const lcd_config_t *config, esp_lcd_panel_io_handle_t *lcd_io, esp_lcd_panel_handle_t *lcd_panel);

// Starts esp_lvgl_port and registers the panel as an LVGL display.
lv_display_t *app_lvgl_init(const lcd_config_t *config, esp_lcd_panel_io_handle_t lcd_io, esp_lcd_panel_handle_t lcd_panel);

#ifdef __cplusplus
}
#endif
