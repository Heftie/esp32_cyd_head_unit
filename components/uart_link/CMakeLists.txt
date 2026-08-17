idf_component_register(SRCS "uart_link.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_uart
                    PRIV_REQUIRES data_hub
                    )
