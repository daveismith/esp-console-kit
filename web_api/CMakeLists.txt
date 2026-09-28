idf_component_register(SRCS "web_fs.c" "web_servo.c" "web_net.c"
                    INCLUDE_DIRS "include"
                    REQUIRES web_server cmd_fs api_core
                    PRIV_REQUIRES servo cmd_wifi esp_wifi esp_netif esp_event lwip events)
