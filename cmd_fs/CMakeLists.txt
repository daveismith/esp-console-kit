idf_component_register(SRCS "cmd_fs.c" "xmodem.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES console esp_driver_uart esp_timer log mbedtls vfs)
