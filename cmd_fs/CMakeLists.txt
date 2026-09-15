idf_component_register(SRCS "cmd_fs.c" "cmd_ota.c" "xfer_session.c" "xmodem.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES app_update console esp_driver_uart esp_partition esp_timer log
                                  mbedtls vfs)
