#pragma once

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_io_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t clk_gpio;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t cs_gpio;
} xpt2046_bitbang_io_config_t;

// Creates an esp_lcd_panel_io_t that speaks the XPT2046's SPI-shaped
// register protocol by bit-banging plain GPIOs instead of a hardware SPI
// peripheral. Only rx_param() is implemented — that's the only entry point
// esp_lcd_touch_xpt2046.c calls — so the handle drops straight into
// esp_lcd_touch_new_spi_xpt2046() unmodified.
esp_err_t xpt2046_bitbang_io_new(const xpt2046_bitbang_io_config_t *config, esp_lcd_panel_io_handle_t *out_io);

#ifdef __cplusplus
}
#endif
