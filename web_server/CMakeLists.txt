idf_component_register(SRCS "web_server.c" "web_ota.c" "ota_pull.c" "web_events.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server esp_app_format cmd_fs espressif__cjson events api_core
                    PRIV_REQUIRES app_update cmd_wifi console esp_http_client esp_netif esp_timer
                                  esp_wifi lwip mbedtls nvs_flash)
