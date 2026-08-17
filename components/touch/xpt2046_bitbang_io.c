#include "xpt2046_bitbang_io.h"

#include <stdlib.h>
#include <sys/cdefs.h>

typedef struct {
    esp_lcd_panel_io_t base; // must be first: esp_lcd_panel_io_handle_t is a pointer to this
    xpt2046_bitbang_io_config_t cfg;
} xpt2046_bitbang_io_t;

static inline void clk_pulse(gpio_num_t clk)
{
    gpio_set_level(clk, 1);
    gpio_set_level(clk, 0);
}

// XPT2046 protocol, SPI mode 0: one CS-bracketed transaction per call,
// clocking out an 8-bit command MSB-first, then clocking in param_size
// bytes MSB-first per byte — same shape esp_lcd_panel_io_rx_param() would
// produce over real SPI (12-bit ADC result left-justified in 16 bits).
// No delay between clock edges: gpio_set_level()'s own call overhead
// already keeps this comfortably under the chip's ~2MHz limit, so this
// runs it as fast as bit-banging over the GPIO driver allows.
static esp_err_t bitbang_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    xpt2046_bitbang_io_t *bio = __containerof(io, xpt2046_bitbang_io_t, base);
    uint8_t *out = (uint8_t *)param;

    gpio_set_level(bio->cfg.cs_gpio, 0);

    for (int i = 7; i >= 0; i--) {
        gpio_set_level(bio->cfg.mosi_gpio, (lcd_cmd >> i) & 1);
        clk_pulse(bio->cfg.clk_gpio);
    }

    for (size_t byte = 0; byte < param_size; byte++) {
        uint8_t value = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_set_level(bio->cfg.clk_gpio, 1);
            value = (uint8_t)((value << 1) | (gpio_get_level(bio->cfg.miso_gpio) & 1));
            gpio_set_level(bio->cfg.clk_gpio, 0);
        }
        out[byte] = value;
    }

    gpio_set_level(bio->cfg.cs_gpio, 1);
    return ESP_OK;
}

static esp_err_t bitbang_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bitbang_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bitbang_register_event_callbacks(esp_lcd_panel_io_t *io, const esp_lcd_panel_io_callbacks_t *cbs, void *user_ctx)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bitbang_del(esp_lcd_panel_io_t *io)
{
    free(__containerof(io, xpt2046_bitbang_io_t, base));
    return ESP_OK;
}

esp_err_t xpt2046_bitbang_io_new(const xpt2046_bitbang_io_config_t *config, esp_lcd_panel_io_handle_t *out_io)
{
    if (config == NULL || out_io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xpt2046_bitbang_io_t *bio = calloc(1, sizeof(*bio));
    if (bio == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bio->cfg = *config;
    bio->base.rx_param = bitbang_rx_param;
    bio->base.tx_param = bitbang_tx_param;
    bio->base.tx_color = bitbang_tx_color;
    bio->base.register_event_callbacks = bitbang_register_event_callbacks;
    bio->base.del = bitbang_del;

    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << config->clk_gpio) | (1ULL << config->mosi_gpio) | (1ULL << config->cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    // No pull-up requested: on this board MISO is GPIO 39, one of the
    // ESP32's input-only ADC pins, which has no internal pull resistor in
    // hardware at all — requesting one just logs a boot-time gpio error.
    // The XPT2046 actively drives this line once CS goes low, so it
    // doesn't need one.
    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << config->miso_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    gpio_set_level(config->clk_gpio, 0); // mode 0 idle: clock low
    gpio_set_level(config->cs_gpio, 1);  // deasserted

    *out_io = &bio->base;
    return ESP_OK;
}
