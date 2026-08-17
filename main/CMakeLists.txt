idf_component_register(SRCS main.c
                    INCLUDE_DIRS "."
                    REQUIRES lcd touch data_hub uart_link rgb_led sd_storage logger web_server
                    )
target_link_libraries(${COMPONENT_LIB} "-u _printf_float")