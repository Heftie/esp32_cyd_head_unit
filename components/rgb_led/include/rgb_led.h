#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t red_gpio;   // GPIO_NUM_NC to leave this channel unconfigured
    gpio_num_t green_gpio; // GPIO_NUM_NC to leave this channel unconfigured
    gpio_num_t blue_gpio;  // GPIO_NUM_NC to leave this channel unconfigured
    bool active_low;       // true if LOW drives the LED on (e.g. stock CYD RGB LED)
} rgb_led_config_t;

// Configures one shared LEDC timer plus one LEDC channel per configured
// color. Channels left as GPIO_NUM_NC in config are skipped and ignored by
// rgb_led_set_rgb().
esp_err_t rgb_led_init(const rgb_led_config_t *config);

// Sets per-channel brightness, 0 (off) to 255 (full brightness).
esp_err_t rgb_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);

// Shorthand for rgb_led_set_rgb(0, 0, 0).
esp_err_t rgb_led_off(void);

#ifdef __cplusplus
}
#endif
