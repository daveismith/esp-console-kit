set(include_dirs
    include
)

set(srcs
    "src/cmd_dig.c"
    "src/cmd_ip.c"
    "src/cmd_iperf.c"
    "src/cmd_network.c"
    "src/cmd_ping.c"
    "src/cmd_traceroute.c"
    "src/iperf.c"
    "src/traceroute.c"
)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "${include_dirs}"
                    REQUIRES console esp_eth esp_netif lwip esp_timer)
