#include "rgb_led.h"

#include <stddef.h>

#include <driver/ledc.h>
#include <esp_log.h>

static const char *TAG = "rgb_led";

#define RGB_LED_LEDC_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define RGB_LED_LEDC_TIMER       LEDC_TIMER_2
#define RGB_LED_LEDC_FREQ_HZ     5000
#define RGB_LED_LEDC_RES_BITS    LEDC_TIMER_8_BIT // matches the uint8_t 0-255 API below

typedef struct {
    ledc_channel_t channel;
    bool configured;
} rgb_led_channel_t;

static rgb_led_channel_t s_red   = { .channel = LEDC_CHANNEL_2 };
static rgb_led_channel_t s_green = { .channel = LEDC_CHANNEL_3 };
static rgb_led_channel_t s_blue  = { .channel = LEDC_CHANNEL_4 };

static esp_err_t init_channel(rgb_led_channel_t *ch, gpio_num_t gpio, bool active_low)
{
    if (gpio == GPIO_NUM_NC) {
        ch->configured = false;
        return ESP_OK;
    }

    const ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = RGB_LED_LEDC_SPEED_MODE,
        .channel = ch->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = RGB_LED_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = active_low,
    };

    esp_err_t err = ledc_channel_config(&cfg);
    if (err == ESP_OK) {
        ch->configured = true;
    }
    return err;
}

static esp_err_t set_channel_duty(rgb_led_channel_t *ch, uint8_t value)
{
    if (!ch->configured) {
        return ESP_OK;
    }

    esp_err_t err = ledc_set_duty(RGB_LED_LEDC_SPEED_MODE, ch->channel, value);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(RGB_LED_LEDC_SPEED_MODE, ch->channel);
}

esp_err_t rgb_led_init(const rgb_led_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->red_gpio == GPIO_NUM_NC && config->green_gpio == GPIO_NUM_NC && config->blue_gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "no channels configured, nothing to do");
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = RGB_LED_LEDC_SPEED_MODE,
        .duty_resolution = RGB_LED_LEDC_RES_BITS,
        .timer_num = RGB_LED_LEDC_TIMER,
        .freq_hz = RGB_LED_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if ((err = init_channel(&s_red, config->red_gpio, config->active_low)) != ESP_OK) {
        return err;
    }
    if ((err = init_channel(&s_green, config->green_gpio, config->active_low)) != ESP_OK) {
        return err;
    }
    if ((err = init_channel(&s_blue, config->blue_gpio, config->active_low)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t rgb_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    esp_err_t err;

    if ((err = set_channel_duty(&s_red, red)) != ESP_OK) {
        return err;
    }
    if ((err = set_channel_duty(&s_green, green)) != ESP_OK) {
        return err;
    }
    if ((err = set_channel_duty(&s_blue, blue)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t rgb_led_off(void)
{
    return rgb_led_set_rgb(0, 0, 0);
}
