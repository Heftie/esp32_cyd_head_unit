idf_component_register(SRCS "ui.c" "screen_nav.c" "touch_test_screen.c" "settings_screen.c" "tiles_screen.c" "measurement_screen.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES lvgl__lvgl espressif__esp_lvgl_port data_hub web_server
                    )
