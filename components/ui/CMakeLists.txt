idf_component_register(SRCS "wifi_settings_screen.c" "time_settings_screen.c" "timezone_screen.c" "log_manager_screen.c" "log_naming.c" "ui.c" "screen_nav.c" "settings_screen.c" "tiles_screen.c" "measurement_screen.c" "set_time_screen.c" "sd_info_screen.c" "graph_screen.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES lvgl__lvgl espressif__esp_lvgl_port data_hub web_server sd_storage logger
                    )
