#pragma once

#include <driver/gpio.h>

#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_BITS_PIXEL     16
#define LCD_BUF_LINES      30
#define LCD_DOUBLE_BUFFER  1
#define LCD_DRAWBUF_SIZE   (LCD_H_RES * LCD_BUF_LINES)

#ifdef CYD_ILI9341
#define LCD_MIRROR_X       (true)
#define LCD_MIRROR_Y       (false)
#else
#define LCD_MIRROR_X       (false)
#define LCD_MIRROR_Y       (false)
#endif

#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_CMD_BITS       (8)
#define LCD_PARAM_BITS     (8)
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_SPI_CLK        (gpio_num_t) GPIO_NUM_14
#define LCD_SPI_MOSI       (gpio_num_t) GPIO_NUM_13
#define LCD_SPI_MISO       (gpio_num_t) GPIO_NUM_12
#define LCD_DC             (gpio_num_t) GPIO_NUM_2
#define LCD_CS             (gpio_num_t) GPIO_NUM_15
#define LCD_RESET          (gpio_num_t) GPIO_NUM_4
#define LCD_BUSY           (gpio_num_t) GPIO_NUM_NC

#define LCD_BACKLIGHT      (gpio_num_t) GPIO_NUM_21
#define LCD_BACKLIGHT_LEDC_CH  (1)

#define TOUCH_X_RES_MIN 0
#define TOUCH_X_RES_MAX 240
#define TOUCH_Y_RES_MIN 0
#define TOUCH_Y_RES_MAX 320

// Bit-banged over plain GPIO (see components/touch/xpt2046_bitbang_io.c),
// not a hardware SPI peripheral — these pins were VSPI/SPI3 in the original
// design, but freeing that peripheral is the whole point: it now belongs to
// sd_storage alone, at full hardware clock, with nothing to share it with.
#define TOUCH_SPI_CLK  (gpio_num_t) GPIO_NUM_25
#define TOUCH_SPI_MOSI (gpio_num_t) GPIO_NUM_32
#define TOUCH_SPI_MISO (gpio_num_t) GPIO_NUM_39
#define TOUCH_CS       (gpio_num_t) GPIO_NUM_33
#define TOUCH_RST      (gpio_num_t) GPIO_NUM_NC
#define TOUCH_IRQ      (gpio_num_t) GPIO_NUM_NC /* GPIO_NUM_36, XPT driver is working better (for me) without IRQ */

#define TOUCH_MIRROR_X (true)
#define TOUCH_MIRROR_Y (false)

// MicroSD card slot (SPI, VSPI/SPI3 — exclusively; see the touch note
// above). Wired to its own physical pins, separate from touch's old ones.
#define SD_SPI_HOST     SPI3_HOST
#define SD_SPI_CLK      (gpio_num_t) GPIO_NUM_18
#define SD_SPI_MOSI     (gpio_num_t) GPIO_NUM_23
#define SD_SPI_MISO     (gpio_num_t) GPIO_NUM_19
#define SD_CS           (gpio_num_t) GPIO_NUM_5
#define SD_MOUNT_POINT  "/sdcard"

// UART link to companion MCU (SCPI-style text protocol, see components/uart_link)
#define UART_LINK_TXD   GPIO_NUM_27
#define UART_LINK_RXD   GPIO_NUM_22
#define UART_LINK_BAUD  115200

// Onboard RGB LED (active LOW). Red is GPIO 4, which this project also
// labels LCD_RESET above — but GPIO 4 isn't actually wired to a display
// reset line (the panel resets via EN at power-on plus the software reset
// command in esp_lcd_panel_init(), see ESP32-CYD-Pinout.md sections 1.1/1.3).
// So driving it as the red channel here is safe and doesn't affect the display.
#define RGB_LED_RED        (gpio_num_t) GPIO_NUM_4
#define RGB_LED_GREEN      (gpio_num_t) GPIO_NUM_16
#define RGB_LED_BLUE       (gpio_num_t) GPIO_NUM_17
#define RGB_LED_ACTIVE_LOW (true)
