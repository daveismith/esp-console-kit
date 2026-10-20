idf_component_register(SRCS "cmd_nvs.c" "nvs_backup.c"
                    INCLUDE_DIRS .
                    REQUIRES console nvs_flash
                    # the file writer and path checks; esp_app_get_description()
                    PRIV_REQUIRES cmd_fs esp_app_format)
