#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include "sdkconfig.h"

#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/dhcp.h"
#include "lwip/stats.h"

#if MIB2_STATS
// Load Up The SNMP HEader
#include "lwip/snmp.h"
#endif // MIB2_STATS

#include "esp_console.h"
#include "argtable3/argtable3.h"

#define REG_EXTENDED 1
#define REG_ICASE (REG_EXTENDED << 1)

static const char *FAMILY_INET = "inet";
static const char *FAMILY_INET6 = "inet6";
#if MIB2_STATS
static const char *FAMILY_LINK = "link";
static const char *FAMILY_REGEX = "inet[6]?|link";
#else
static const char *FAMILY_REGEX = "inet[6]?";
#endif //!MIB2_STATS

static struct {
    struct arg_lit *statistics;
    struct arg_rex *family;
    struct arg_rex *object;

    // Command
    struct arg_end *end;
} ip_addr_args;

/**
 * ip addr
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host 
       valid_lft forever preferred_lft forever
2: ens33: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000
    link/ether 00:0c:29:9d:bf:54 brd ff:ff:ff:ff:ff:ff
    altname enp2s1
    inet 10.0.19.133/24 brd 10.0.19.255 scope global dynamic noprefixroute ens33
       valid_lft 80839sec preferred_lft 80839sec
    inet6 2607:f2c0:e368:400:c795:5add:fd85:b23c/64 scope global temporary dynamic 
       valid_lft 83812sec preferred_lft 80316sec
    inet6 2607:f2c0:e368:400:20c:29ff:fe9d:bf54/64 scope global dynamic mngtmpaddr 
       valid_lft 83812sec preferred_lft 83812sec
    inet6 fe80::20c:29ff:fe9d:bf54/64 scope link 
       valid_lft forever preferred_lft forever

*/

/**
 * ip -s addr
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host 
       valid_lft forever preferred_lft forever
    RX:  bytes packets errors dropped  missed   mcast           
        104610    1072      0       0       0       0 
    TX:  bytes packets errors dropped carrier collsns           
        104610    1072      0       0       0       0 
2: ens33: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000
    link/ether 00:0c:29:9d:bf:54 brd ff:ff:ff:ff:ff:ff
    altname enp2s1
    inet 10.0.19.133/24 brd 10.0.19.255 scope global dynamic noprefixroute ens33
       valid_lft 78746sec preferred_lft 78746sec
    inet6 2607:f2c0:e368:400:c795:5add:fd85:b23c/64 scope global temporary dynamic 
       valid_lft 81719sec preferred_lft 78223sec
    inet6 2607:f2c0:e368:400:20c:29ff:fe9d:bf54/64 scope global dynamic mngtmpaddr 
       valid_lft 81719sec preferred_lft 81719sec
    inet6 fe80::20c:29ff:fe9d:bf54/64 scope link 
       valid_lft forever preferred_lft forever
    RX:  bytes packets errors dropped  missed   mcast           
     127332621   91072      0       0       0       0 
    TX:  bytes packets errors dropped carrier collsns           
       1889648   22642      0       0       0       0 
*/

typedef struct {
    uint8_t flag;
    char *flag_name;
} flag_t;

static void print_flags(uint8_t flags)
{
    flag_t order[] = {
        {NETIF_FLAG_BROADCAST, "BROADCAST"},
        {NETIF_FLAG_UP, "UP"}, 
        {NETIF_FLAG_LINK_UP, "LOWER_UP"},
        {NETIF_FLAG_ETHARP, "ETHARP"},
        {NETIF_FLAG_ETHERNET, "ETH"},
        {NETIF_FLAG_IGMP, "IGMP"},
        {NETIF_FLAG_MLD6, "MLD6"}
    };

    bool needs_comma = false;
    for (size_t idx = 0; idx < sizeof(order) / sizeof(flag_t); idx++) 
    {
        flag_t *flag = &order[idx];
        if ((flags & flag->flag) == 0) 
        {
            continue;
        }

        if (needs_comma) 
        {
            printf(",");
        }
        needs_comma = true;

        printf("%s", flag->flag_name);
    }
}

static uint8_t netmask_to_cidr(const ip4_addr_t *netmask) {
    uint8_t cidr = 0;
    uint8_t i;
    uint32_t addr = ntohl(netmask->addr);

    for (i = 0; i < 32; i++) {
        if ((addr & (1 << (31 - i))) == 0) {
            break;
        }
        cidr++;
    }

    return cidr;
}

static ip4_addr_t get_broadcast_address(const ip4_addr_t *addr, const ip4_addr_t *netmask) {
    uint32_t my_addr = ntohl(addr->addr);
    uint32_t my_netmask = ntohl(netmask->addr);
    ip4_addr_t broadcast;

    /* Calculate the broadcast address */
    broadcast.addr = htonl((my_addr & my_netmask) | ~my_netmask);

    return broadcast;
}

static void ip4_addr_print_scope(const ip4_addr_t *addr) 
{
    printf("scope ");
    
    if (ip4_addr_isloopback(addr))
    {
        printf("host");
    } else if (ip4_addr_islinklocal(addr)) {
        printf("link");
    } else {
        printf("global");
    }
}

static void ip6_addr_print_scope(const ip6_addr_t *addr, const uint8_t addr_state)
{
    printf("scope ");

    if (ip6_addr_isloopback(addr)) {
        printf("host");
        return;
    } else if (ip6_addr_islinklocal(addr)) {
        printf("link");
        return;
    } else if (ip6_addr_issitelocal(addr)) {
        printf("site");
    } else if (ip6_addr_isglobal(addr)) {
        printf("global");
    }

    // flags
    // permanent | dynamic | secondary | primary | [-]tentative | [-]deprecated | [-]dadfailed | temporary
    
    /*if (is_slaac_address(addr)) {
        printf(" dynamic");
    } else {
        printf(" permanent");
    }*/

    // primary/secondary are not supported by LWIP


    if (ip6_addr_isdeprecated(addr_state)) {
        printf(" deprecated");
    } 

    if (!ip6_addr_isvalid(addr_state) && ip6_addr_istentative(addr_state)) {
        printf(" tentative");
    }
    
    if (ip6_addr_isduplicated(addr_state)) {
        printf(" dadfailed");
    }

    if (ip6_addr_isvalid(addr_state) && ip6_addr_istentative(addr_state)) {
        printf(" temporary");
    }

    // confflag
    //home | mngtmpaddr | nodad | noprefixroute | autojoin


    /*
    inet6 fdc3:464e:1829:46dc:5c88:69ab:b9bf:b7e3/64 scope global deprecated dynamic mngtmpaddr noprefixroute    
    inet6 2607:f2c0:e368:400:c795:5add:fd85:b23c/64 scope global temporary dynamic 
    inet6 2607:f2c0:e368:400:20c:29ff:fe9d:bf54/64 scope global dynamic mngtmpaddr 
    inet6 fe80::20c:29ff:fe9d:bf54/64 scope link 
    inet6 ::1/128 scope host
    */
}

#if MIB2_STATS
static void display_link(struct netif *my_netif)
{
    const char *link_type = "";
    switch (my_netif->link_type) {
    case snmp_ifType_ethernet_csmacd:
        link_type = "ether";
        break;
    case snmp_ifType_softwareLoopback:
        link_type = "loopback";
        break;
    }
    printf("    link/%s\n", link_type);
    
    //TODO: Mac Address
    //TODO: Broadcast Mac Address

    /*
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    link/ether dc:a6:32:00:d7:fd brd ff:ff:ff:ff:ff:ff
    link/ether dc:a6:32:00:d7:fe brd ff:ff:ff:ff:ff:ff
    link/ether 02:42:bc:cd:b6:c2 brd ff:ff:ff:ff:ff:ff
    */
}

static void display_stats(struct netif *my_netif)
{
    printf("    RX:  bytes packets errors dropped  missed   mcast\n");
    printf("       %7"PRIu32" %7"PRIu32" %6"PRIu32" %7"PRIu32" %7"PRIu32" %7"PRIu32"\n",
        my_netif->mib2_counters.ifinoctets,
        my_netif->mib2_counters.ifinucastpkts + my_netif->mib2_counters.ifinnucastpkts,
        my_netif->mib2_counters.ifinerrors,
        my_netif->mib2_counters.ifindiscards,
        0l,
        0l);
    printf("    TX:  bytes packets errors dropped carrier collsns\n");
    printf("       %7"PRIu32" %7"PRIu32" %6"PRIu32" %7"PRIu32" %7"PRIu32" %7"PRIu32"\n",
        my_netif->mib2_counters.ifoutoctets,
        my_netif->mib2_counters.ifoutucastpkts + my_netif->mib2_counters.ifoutnucastpkts,
        my_netif->mib2_counters.ifouterrors,
        my_netif->mib2_counters.ifoutdiscards,
        0l,
        0l);
    /*
    RX:  bytes packets errors dropped  missed   mcast           
        104610    1072      0       0       0       0 
    TX:  bytes packets errors dropped carrier collsns           
        104610    1072      0       0       0       0 
    */
}
#endif //MIB2_STATS

#if LWIP_IPV4
static void display_ipv4(const char *if_name, struct netif *netif)
{
    const ip_addr_t *addr = netif_ip_addr4(netif);
    const ip4_addr_t *netmask = netif_ip4_netmask(netif);
    const ip4_addr_t broadcast = get_broadcast_address(ip_2_ip4(addr), netmask);
    uint8_t cidr = netmask_to_cidr(netmask);

    printf("    %s %s/%"PRIu8" ", FAMILY_INET, ipaddr_ntoa(addr), cidr);

    if (netif->flags & NETIF_FLAG_BROADCAST)
    {
        printf("brd %s ", ip4addr_ntoa(&broadcast));
    }

    ip4_addr_print_scope(ip_2_ip4(addr));
    printf(" ");
    
    // If handle "dynamic", "noprefixroute"
    //printf("?? ?? ");
    printf("%s \n", if_name);
    printf("        valid_lft forever preferred_lft forever\n");
}
#endif //LWIP_IPV4

#if LWIP_IPV6
static void display_ipv6(const char *if_name, struct netif *netif)
{
    char ip_str[IP6ADDR_STRLEN_MAX];
    for (size_t idx = 0; idx < LWIP_IPV6_NUM_ADDRESSES; idx++) 
    {
        memset(ip_str, 0, sizeof(ip_str));
        const ip_addr_t *addr = &netif->ip6_addr[idx];
        uint8_t state = netif->ip6_addr_state[idx];
#if LWIP_IPV6_ADDRESS_LIFETIMES                
        u32_t ip6_addr_valid_life = netif->ip6_addr_valid_life[idx];
        u32_t ip6_addr_pref_life = netif->ip6_addr_pref_life[idx];
#endif // LWIP_IPV6_ADDRESS_LIFETIMES

        if (ip6_addr_isinvalid(state)) { continue; }  // Don't display invalid address

        // Get the IPv6 address and then transform to lowercase
        ipaddr_ntoa_r(addr, ip_str, sizeof(ip_str));
        for (size_t str_idx = 0; ip_str[str_idx] != '\0'; str_idx++) {
            ip_str[str_idx] = (char)tolower((unsigned char)ip_str[str_idx]);
        }

        printf("    %s %s ", FAMILY_INET6, ip_str);
        ip6_addr_print_scope(ip_2_ip6(addr), state);
        printf("\n");

#if LWIP_IPV6_ADDRESS_LIFETIMES
        printf("        valid_lft ");
        if (ip6_addr_life_isstatic(ip6_addr_valid_life))
        {
            printf("forever");
        } else {
            printf("%"PRIu32"sec", ip6_addr_valid_life);
        }
        printf(" preferred_lft ");
        if (ip6_addr_life_isstatic(ip6_addr_pref_life))
        {
            printf("forever");
        } else {
            printf("%"PRIu32"sec", ip6_addr_pref_life);
        }
        printf("\n");
#endif //LWIP_IPV6_ADDRESS_LIFETIMES

//inet6 ::1/128 scope host 
//   valid_lft forever preferred_lft forever
//inet6 2607:f2c0:e368:400:c795:5add:fd85:b23c/64 scope global temporary dynamic 
//   valid_lft 81719sec preferred_lft 78223sec
//inet6 2607:f2c0:e368:400:20c:29ff:fe9d:bf54/64 scope global dynamic mngtmpaddr 
//   valid_lft 81719sec preferred_lft 81719sec
//inet6 fe80::20c:29ff:fe9d:bf54/64 scope link 
//   valid_lft forever preferred_lft forever    
    }
}
#endif 

static int do_ip_addr_cmd(int argc, char **argv)
{
    struct netif *netif;
    char if_name[NETIF_NAMESIZE];

    if (netif_list == NULL) {
        printf("no network interfaces\n");
        return 0;
    }
    uint8_t max_netif_num = netif_get_index(netif_list); // The first item in the list is the highest number

    // Default display to true if the -f/--family
    bool show_ipv4 = (ip_addr_args.family->count == 0);
    bool show_ipv6 = (ip_addr_args.family->count == 0);
#if MIB2_STATS
    bool show_link = (ip_addr_args.family->count == 0);
#endif

    // Check If There's A Family Configured
    if (ip_addr_args.family->count > 0) {
        const char *val = ip_addr_args.family->sval[0];
        // Compare to see if the option is selected. Note that the +1 is present to ensure the strings are equal with NULL termination
        show_ipv4 = (0 == strncmp(FAMILY_INET, val, strlen(FAMILY_INET) + 1));
        show_ipv6 = (0 == strncmp(FAMILY_INET6, val, strlen(FAMILY_INET6) + 1));
#if MIB2_STATS
        show_link = (0 == strncmp(FAMILY_LINK, val, strlen(FAMILY_LINK) + 1));
#endif
    }

    LOCK_TCPIP_CORE();
    // Zero Is Not Permitted
    for (uint8_t idx = 1; idx <= max_netif_num; idx++)
    {
        // Get the LWIP netif. Indices are not dense: an interface that has been removed
        // leaves a hole, and netif_get_by_index() hands back NULL for it.
        netif = netif_get_by_index(idx);
        if (netif == NULL) {
            continue;
        }

        // Header Line
        memset(if_name, 0, sizeof(if_name));
        netif_index_to_name(idx, if_name);
        printf("%d: %s <", idx, if_name);
        print_flags(netif->flags);
        printf("> ");
        printf("mtu %u ", netif->mtu);
        // mtu6?
        printf("state %s ", (netif->flags & NETIF_FLAG_UP) ? "UP" : "DOWN");
        // group, default, qlen, 
        printf("\n");

        // Get Link Type (use MIB2_STATS)
#if MIB2_STATS
        if (show_link) { display_link(netif); }
#endif //MIB2_STATS
        
        // Get IP4 Addresses
#if LWIP_IPV4
        if (show_ipv4) { display_ipv4(if_name, netif); }
#endif //LWIP_IPV4

        // Get IP6 Addresses
#if LWIP_IPV6
        if (show_ipv6) { display_ipv6(if_name, netif); }
#endif //LWIP_IPV4

        // Statistics?
#if MIB2_STATS
        display_stats(netif);
#endif //MIB2_STATS

    }
    UNLOCK_TCPIP_CORE();

    return 0;
}

static int do_ip_cmd(int argc, char **argv)
{
    int addr_nerrors = arg_parse(argc, argv, (void **)&ip_addr_args);

    if (0 == addr_nerrors) {
        return do_ip_addr_cmd(argc, argv);
    }
    // Add Other Commands

    arg_print_errors(stderr, ip_addr_args.end, argv[0]);
    return 1;
}



void register_ip(void)
{
    ip_addr_args.statistics = arg_lit0("s", "statistics", "Output more info");
    ip_addr_args.family = arg_rex0("f", "family", FAMILY_REGEX, NULL, 0, "protocol family");
    ip_addr_args.object = arg_rex1(NULL, NULL, "addr(ess)?", NULL, REG_ICASE, NULL);
    ip_addr_args.end = arg_end(1);

    const esp_console_cmd_t ip_cmd = {
        .command = "ip",
        .help = "show / manipulate routing, network devices, interfaces and tunnels",
        .hint = NULL,
        .func = &do_ip_cmd,
        .argtable = &ip_addr_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ip_cmd));
    stats_init();
}