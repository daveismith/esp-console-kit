#include "esp_console.h"
#include "cmd_dig.h"
#include "cmd_ip.h"
#include "cmd_iperf.h"
#include "cmd_ping.h"
#include "cmd_traceroute.h"

void register_network_commands(void)
{
    register_dig();
    register_ip();
    register_iperf();
    register_ping();
    register_traceroute();
}