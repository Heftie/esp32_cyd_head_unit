idf_component_register(SRCS "web_server.c"
                    INCLUDE_DIRS "include"
                    EMBED_TXTFILES "index.html"
                    REQUIRES esp_wifi esp_netif esp_event esp_http_server data_hub sd_storage
                    PRIV_REQUIRES nvs_flash esp_timer espressif__mdns espressif__cjson
                    )
