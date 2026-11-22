# The Cyphal node: libcanard, the committed Nunavut headers in dsdl/ (tools/gen_dsdl.py
# regenerates them), and the board's CAN controller through canbus. Nothing of any one board's.
# dsdl/ is public: the register table's header uses its types. An application with types of its
# own (holo-player-fw's r2.holo) keeps their headers in a directory of its own and builds its
# port table from them.
idf_component_register(SRCS "cyphal.c" "cmd_cyphal.c" "cyphal_util.c" "cyphal_registers.c"
                       INCLUDE_DIRS "include" "dsdl"
                       PRIV_REQUIRES canbus libcanard console nvs_flash esp_timer esp_app_format esp_hw_support
                                     heap freertos log)
