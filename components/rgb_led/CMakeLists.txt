idf_component_register(SRCS "rgb_led.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_ledc esp_driver_gpio
                    )
