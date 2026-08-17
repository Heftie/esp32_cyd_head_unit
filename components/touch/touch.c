#include "touch.h"

#include <esp_log.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_xpt2046.h>

#include "xpt2046_bitbang_io.h"

static touch_config_t s_config;

static uint16_t map(uint16_t n, uint16_t in_min, uint16_t in_max, uint16_t out_min, uint16_t out_max)
{
    uint16_t value = (n - in_min) * (out_max - out_min) / (in_max - in_min);
    return (value < out_min) ? out_min : ((value > out_max) ? out_max : value);
}

static void process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    *x = map(*x, s_config.x_res_min, s_config.x_res_max, 0, s_config.h_res);
    *y = map(*y, s_config.y_res_min, s_config.y_res_max, 0, s_config.v_res);
}

esp_err_t touch_init(const touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    s_config = *config;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const xpt2046_bitbang_io_config_t bitbang_cfg = {
        .clk_gpio = config->spi_clk_gpio,
        .mosi_gpio = config->spi_mosi_gpio,
        .miso_gpio = config->spi_miso_gpio,
        .cs_gpio = config->cs_gpio,
    };
    ESP_ERROR_CHECK(xpt2046_bitbang_io_new(&bitbang_cfg, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {.x_max = config->h_res,
                                   .y_max = config->v_res,
                                   .rst_gpio_num = config->rst_gpio,
                                   .int_gpio_num = config->irq_gpio,
                                   .levels = {.reset = 0, .interrupt = 0},
                                   .flags =
                                       {
                                           .swap_xy = false,
                                           .mirror_x = config->mirror_x,
                                           .mirror_y = config->mirror_y
                                       },
                                   .process_coordinates = process_coordinates,
                                   .interrupt_callback = NULL};

    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, tp));

    return ESP_OK;
}
