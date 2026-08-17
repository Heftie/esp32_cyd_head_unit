#include "lcd.h"

#include <esp_check.h>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>

#ifdef CYD_ILI9341
#include <esp_lcd_ili9341.h>
#endif

static const char *TAG = "lcd";

static ledc_channel_t s_backlight_channel;

esp_err_t lcd_display_brightness_init(const lcd_config_t *config)
{
    s_backlight_channel = config->backlight_ledc_channel;

    const ledc_channel_config_t backlight_channel_cfg = {
        .gpio_num = config->backlight_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = config->backlight_ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = false
    };

    const ledc_timer_config_t backlight_timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer_cfg));
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel_cfg));

    return ESP_OK;
}

esp_err_t lcd_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);

    uint32_t duty_cycle = (1023 * brightness_percent) / 100;

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_backlight_channel, duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_backlight_channel));

    return ESP_OK;
}

esp_err_t lcd_display_backlight_off(void)
{
    return lcd_display_brightness_set(0);
}

esp_err_t lcd_display_backlight_on(void)
{
    return lcd_display_brightness_set(100);
}

esp_err_t lcd_display_rotate(lv_display_t *lvgl_disp, lv_display_rotation_t dir)
{
    if (lvgl_disp)
    {
        lv_display_set_rotation(lvgl_disp, dir);
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t app_lcd_init(const lcd_config_t *config, esp_lcd_panel_io_handle_t *lcd_io, esp_lcd_panel_handle_t *lcd_panel)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = config->spi_mosi_gpio,
        .miso_io_num = config->spi_miso_gpio,
        .sclk_io_num = config->spi_clk_gpio,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = config->h_res * config->draw_buf_lines * sizeof(uint16_t),
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->spi_host, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");


    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = config->cs_gpio,
        .dc_gpio_num = config->dc_gpio,
        .spi_mode = 0,
        .pclk_hz = config->pixel_clock_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = config->lcd_cmd_bits,
        .lcd_param_bits = config->lcd_param_bits,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config->spi_host, &io_config, lcd_io), TAG, "LCD new panel io failed");


    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = config->reset_gpio,
        #ifdef CYD_ILI9341
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        #else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        #endif
        .bits_per_pixel = config->bits_per_pixel,
    };

    #ifdef CYD_ILI9341
    esp_err_t r = esp_lcd_new_panel_ili9341(*lcd_io, &panel_config, lcd_panel);
    #else
    esp_err_t r = esp_lcd_new_panel_st7789(*lcd_io, &panel_config, lcd_panel);
    #endif


    esp_lcd_panel_reset(*lcd_panel);
    esp_lcd_panel_init(*lcd_panel);

    esp_lcd_panel_mirror(*lcd_panel, config->mirror_x, config->mirror_y);
    esp_lcd_panel_disp_on_off(*lcd_panel, true);

    return r;
}


lv_display_t *app_lvgl_init(const lcd_config_t *config, esp_lcd_panel_io_handle_t lcd_io, esp_lcd_panel_handle_t lcd_panel)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5
    };

    esp_err_t e = lvgl_port_init(&lvgl_cfg);

    if (e != ESP_OK)
    {
        ESP_LOGI(TAG, "lvgl_port_init() failed: %s", esp_err_to_name(e));

        return NULL;
    }


    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = config->h_res * config->draw_buf_lines,
        .double_buffer = config->double_buffer,
        .hres = config->h_res,
        .vres = config->v_res,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = config->mirror_x,
            .mirror_y = config->mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        }
    };

    return lvgl_port_add_disp(&disp_cfg);
}
