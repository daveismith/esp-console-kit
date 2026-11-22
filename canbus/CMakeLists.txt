idf_component_register(SRCS "canbus.c" "cmd_can.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES esp_driver_twai esp_driver_gpio esp_timer console heap)
