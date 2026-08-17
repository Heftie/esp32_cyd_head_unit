idf_component_register(SRCS demo.c
                    INCLUDE_DIRS "."
                    REQUIRES lcd touch data_hub uart_link rgb_led sd_storage logger
                    )
target_link_libraries(${COMPONENT_LIB} "-u _printf_float")