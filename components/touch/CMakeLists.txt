idf_component_register(SRCS "xpt2046_bitbang_io.c" "touch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio espressif__esp_lcd_touch
                    PRIV_REQUIRES atanisoft__esp_lcd_touch_xpt2046
                    )
