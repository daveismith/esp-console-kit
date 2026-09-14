if(CONFIG_ESP_WIFI_ENABLED OR CONFIG_ESP_HOST_WIFI_ENABLED)
    set(srcs "cmd_wifi.c" "wifi_known.c")
else()
    set(srcs)
endif()

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS .
                    REQUIRES console esp_wifi esp_netif esp_event
                    PRIV_REQUIRES nvs_flash esp_timer)
