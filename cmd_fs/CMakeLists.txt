idf_component_register(SRCS "ota_core.c" "cmd_fs.c" "cmd_ota.c" "xfer_session.c" "xmodem.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update esp_app_format
                    PRIV_REQUIRES console esp_driver_uart esp_partition esp_timer log bootloader_support
                                  mbedtls vfs)
