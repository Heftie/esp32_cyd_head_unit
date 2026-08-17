idf_component_register(SRCS demo.c lcd.c touch.c
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_ledc data_hub uart_link rgb_led
                    )
target_link_libraries(${COMPONENT_LIB} "-u _printf_float")