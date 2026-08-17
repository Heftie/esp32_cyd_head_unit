idf_component_register(SRCS "touch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_driver_spi espressif__esp_lcd_touch
                    PRIV_REQUIRES atanisoft__esp_lcd_touch_xpt2046
                    )
