# The Cyphal node: libcanard, the committed Nunavut headers in dsdl/ (tools/gen_dsdl.py
# regenerates them), and the board's CAN controller through canbus. Nothing of any one board's.
# dsdl/ is public: the register table's header uses its types. An application with types of its
# own (holo-player-fw's r2.holo) keeps their headers in a directory of its own and builds its
# port table from them.
# The plug-and-play allocator's plain parts (cyphal_alloc.c) only with CONFIG_CYPHAL_PNP_ALLOCATOR, so
# that without it the library is what it was, object for object.
set(srcs "cyphal.c" "cmd_cyphal.c" "cyphal_util.c" "cyphal_registers.c")
if(CONFIG_CYPHAL_PNP_ALLOCATOR)
    list(APPEND srcs "cyphal_alloc.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include" "dsdl"
                       PRIV_REQUIRES canbus libcanard console nvs_flash esp_timer esp_app_format esp_hw_support
                                     heap freertos log)
