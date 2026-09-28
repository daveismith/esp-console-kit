idf_component_register(SRCS "api_core.c"
                    INCLUDE_DIRS "include"
                    REQUIRES espressif__cjson
                    PRIV_REQUIRES log)
