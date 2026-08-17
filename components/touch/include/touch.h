#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Bit-banged over plain GPIOs (see xpt2046_bitbang_io.c) rather than a
    // hardware SPI peripheral, so these pins never tie up SPI2 or SPI3 —
    // SPI2 stays the LCD's, and SPI3 (this touch controller's old bus) is
    // free for sd_storage to own exclusively, at full hardware clock.
    gpio_num_t spi_clk_gpio;
    gpio_num_t spi_mosi_gpio;
    gpio_num_t spi_miso_gpio;
    gpio_num_t cs_gpio;

    gpio_num_t rst_gpio;
    gpio_num_t irq_gpio;

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

// Brings up the XPT2046 touch controller over bit-banged GPIO. Registers a
// process_coordinates callback that rescales raw XPT2046 readings (per
// config->x_res_*/y_res_*) onto config->h_res/v_res.
esp_err_t touch_init(const touch_config_t *config, esp_lcd_touch_handle_t *tp);

#ifdef __cplusplus
}
#endif
