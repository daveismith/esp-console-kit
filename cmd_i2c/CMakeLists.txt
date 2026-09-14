idf_component_register(SRCS "cmd_i2ctools.c"
                    INCLUDE_DIRS .
                    # IDF 6.x split the monolithic `driver` component: driver/i2c_master.h
                    # now comes from esp_driver_i2c, and `driver` carries only the legacy
                    # i2c/touch/twai drivers. argtable3 and esp_console come from `console`.
                    PRIV_REQUIRES console esp_driver_i2c)
