idf_component_register(SRCS "events.c"
                    INCLUDE_DIRS "include"
                    REQUIRES espressif__cjson
                    PRIV_REQUIRES esp_timer)
