#include "touch.h"

#include <esp_log.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_xpt2046.h>

#include <driver/spi_master.h>
#include <driver/gpio.h>

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

    const esp_lcd_panel_io_spi_config_t tp_io_config = { .cs_gpio_num = config->cs_gpio,
        .dc_gpio_num = config->dc_gpio,
        .spi_mode = 0,
        .pclk_hz = config->clock_hz,
        .trans_queue_depth = 3,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = { .dc_low_on_data = 0, .octal_mode = 0, .sio_mode = 0, .lsb_first = 0, .cs_high_active = 0 } };

    static const int SPI_MAX_TRANSFER_SIZE = 32768;
    const spi_bus_config_t buscfg_touch = { .mosi_io_num = config->spi_mosi_gpio,
        .miso_io_num = config->spi_miso_gpio,
        .sclk_io_num = config->spi_clk_gpio,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SPI_MAX_TRANSFER_SIZE,
        .flags = SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MISO | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM };

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

    ESP_ERROR_CHECK(spi_bus_initialize(config->spi_host, &buscfg_touch, SPI_DMA_CH_AUTO));

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config->spi_host, &tp_io_config, &tp_io_handle));
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, tp));

    return ESP_OK;
}
