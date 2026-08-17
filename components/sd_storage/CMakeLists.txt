idf_component_register(SRCS "sd_storage.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_driver_spi
                    PRIV_REQUIRES esp_driver_sdspi sdmmc fatfs
                    )
