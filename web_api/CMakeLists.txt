idf_component_register(SRCS "web_fs.c" "web_servo.c"
                    INCLUDE_DIRS "include"
                    REQUIRES web_server cmd_fs
                    PRIV_REQUIRES servo)
