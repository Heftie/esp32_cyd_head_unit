#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // SPI bus shared by the touch controller
    spi_host_device_t spi_host;
    gpio_num_t spi_clk_gpio;
    gpio_num_t spi_mosi_gpio;
    gpio_num_t spi_miso_gpio;

    // Controller lines (GPIO_NUM_NC where unused)
    gpio_num_t cs_gpio;
    gpio_num_t dc_gpio;
    gpio_num_t rst_gpio;
    gpio_num_t irq_gpio;

    int clock_hz;

    // Target display resolution the raw touch reading is scaled onto
    int h_res;
    int v_res;
    bool mirror_x;
    bool mirror_y;

    // Raw controller reading range, mapped onto [0, h_res) / [0, v_res)
    uint16_t x_res_min;
    uint16_t x_res_max;
    uint16_t y_res_min;
    uint16_t y_res_max;
} touch_config_t;

// Brings up the touch controller's SPI bus and driver. Registers a
// process_coordinates callback that rescales raw XPT2046 readings (per
// config->x_res_*/y_res_*) onto config->h_res/v_res.
esp_err_t touch_init(const touch_config_t *config, esp_lcd_touch_handle_t *tp);

#ifdef __cplusplus
}
#endif
