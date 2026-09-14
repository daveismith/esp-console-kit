#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_err.h"
#include "traceroute.h"

#include "freertos/event_groups.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"


static struct {
    struct arg_int *first_ttl;
    struct arg_int *m_first_ttl;
    struct arg_int *max_ttl;
    struct arg_int *nqueries;
    struct arg_int *tos;
    struct arg_str *host;

    // Command
    struct arg_end *end;
} traceroute_args;

typedef struct {
    esp_traceroute_config_t config; /*!< Config*/
    char *hostname;             /*!< Hostname */
    ip_addr_t target_addr;      /*!< Target IP address, either IPv4 or IPv6 */

    int last_ttl;               /*!< Last TTL */
    ip_addr_t last_response;    /*!< Last Response IP address, either IPv4 or IPv6 */
    // attempt?
} traceroute_instance_t;

static EventGroupHandle_t traceroute_events = NULL;

#define TRACEROUTE_RUNNING (1 << 0)
#define TRACEROUTE_COMPLETE (1 << 1)

static void cmd_traceroute_on_traceroute_success(esp_traceroute_handle_t hdl, void *args)
{
    traceroute_instance_t *inst = (traceroute_instance_t *)args;
    int ttl;
    ip_addr_t target_addr;
    uint64_t hop_us;
    esp_traceroute_get_profile(hdl, ESP_TRACEROUTE_PROF_TTL, &ttl, sizeof(ttl));
    esp_traceroute_get_profile(hdl, ESP_TRACEROUTE_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_traceroute_get_profile(hdl, ESP_TRACEROUTE_PROF_TIMEGAP, &hop_us, sizeof(hop_us));
    
    if (ttl != inst->last_ttl) {
        printf("\n%2d  ? (%s)", ttl, ipaddr_ntoa(&target_addr));
        inst->last_ttl = ttl;
    } else if (!ip_addr_cmp(&inst->last_response, &target_addr)) {
        printf("\n    ? (%s)", ipaddr_ntoa(&target_addr));
    }
    memcpy(&inst->last_response, &target_addr, sizeof(ip_addr_t));

    // Output The Time
    printf("  %"PRIu64".%03"PRIu64"ms",
        hop_us / 1000,
        hop_us % 1000);
}

static void cmd_traceroute_on_traceroute_timeout(esp_traceroute_handle_t hdl, void *args)
{
    printf("timeout\n");
}

static void cmd_traceroute_on_traceroute_start(esp_traceroute_handle_t hdl, void *args)
{
    traceroute_instance_t *inst = (traceroute_instance_t *)args;

    printf("traceroute to %s (%s), %d hops max", inst->hostname, ipaddr_ntoa(&inst->target_addr), inst->config.max_ttl);
}

static void cmd_traceroute_on_traceroute_end(esp_traceroute_handle_t hdl, void *args)
{
    traceroute_instance_t *inst = (traceroute_instance_t *)args;
    printf("\nend\n");

    if (NULL != inst) {
        if (NULL != inst->hostname) {
            free(inst->hostname);
            inst->hostname = NULL;
        }

        free(inst);
    }
    xEventGroupSetBits(traceroute_events, TRACEROUTE_COMPLETE);
}

static int do_traceroute_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&traceroute_args);
    EventBits_t bits = xEventGroupGetBits(traceroute_events);

    if (nerrors != 0) {
        arg_print_errors(stderr, traceroute_args.end, argv[0]);
        return 1;
    }

    if ((bits & TRACEROUTE_RUNNING) != 0) {
        printf("traceroute: traceroute in progress\n");
        return 1;
    }
    xEventGroupSetBits(traceroute_events, TRACEROUTE_RUNNING);

    esp_traceroute_config_t config = ESP_TRACEROUTE_DEFAULT_CONFIG();
    traceroute_instance_t *inst = calloc(1, sizeof(traceroute_instance_t));
    if (NULL == inst) {
        printf("traceroute: out of memory\n");
        xEventGroupClearBits(traceroute_events, TRACEROUTE_RUNNING);
        return 1;
    }
    
    // Add Other Commands
    if (traceroute_args.first_ttl->count > 0) {
        config.first_ttl = traceroute_args.first_ttl->ival[0];
    }

    if (traceroute_args.m_first_ttl->count > 0) {
        config.first_ttl = traceroute_args.m_first_ttl->ival[0];
    }

    if (traceroute_args.max_ttl->count > 0) {
        config.max_ttl = traceroute_args.max_ttl->ival[0];
    }

    if (traceroute_args.nqueries->count > 0) {
        config.nqueries = (uint32_t)traceroute_args.nqueries->ival[0];
    }

    if (traceroute_args.tos->count > 0) {
        config.tos = traceroute_args.tos->ival[0];
    }

    // parse IP address
    struct sockaddr_in6 sock_addr6;
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    size_t len = strlen(traceroute_args.host->sval[0]);
    inst->hostname = calloc(1, len+1);
    if (NULL == inst->hostname) {
        printf("traceroute: out of memory\n");
        goto exit;
    }
    memcpy(inst->hostname, traceroute_args.host->sval[0], len);

    if (inet_pton(AF_INET6, traceroute_args.host->sval[0], &sock_addr6.sin6_addr) == 1) {
        /* convert ip6 string to ip6 address */
        ipaddr_aton(traceroute_args.host->sval[0], &target_addr);
    } else {
        struct addrinfo hint;
        struct addrinfo *res = NULL;
        memset(&hint, 0, sizeof(hint));
        /* convert ip4 string or hostname to ip4 or ip6 address */
        if (getaddrinfo(traceroute_args.host->sval[0], NULL, &hint, &res) != 0) {
            printf("traceroute: unknown host %s\n", traceroute_args.host->sval[0]);
            goto exit;
        }
        if (res->ai_family == AF_INET) {
            struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
        } else {
            struct in6_addr addr6 = ((struct sockaddr_in6 *) (res->ai_addr))->sin6_addr;
            inet6_addr_to_ip6addr(ip_2_ip6(&target_addr), &addr6);
        }
        freeaddrinfo(res);
    }
    memcpy(&config.target_addr, &target_addr, sizeof(ip_addr_t));
    memcpy(&inst->target_addr, &target_addr, sizeof(ip_addr_t));
    memcpy(&inst->config, &config, sizeof(esp_traceroute_config_t));
    
    esp_traceroute_callbacks_t cbs = {
        .cb_args = inst,
        .on_traceroute_success = cmd_traceroute_on_traceroute_success,
        .on_traceroute_timeout = cmd_traceroute_on_traceroute_timeout,
        .on_traceroute_start = cmd_traceroute_on_traceroute_start,
        .on_traceroute_end = cmd_traceroute_on_traceroute_end
    };

    esp_traceroute_handle_t traceroute = NULL;
    esp_err_t err = esp_traceroute_new_session(&inst->config, &cbs, &traceroute);
    if (err != ESP_OK) {
        printf("traceroute: cannot create session: %s\n", esp_err_to_name(err));
        goto exit;
    }
    err = esp_traceroute_start(traceroute);
    if (err != ESP_OK) {
        printf("traceroute: cannot start session: %s\n", esp_err_to_name(err));
        esp_traceroute_delete_session(traceroute);
        goto exit;
    }

    // Need to capture CTRL+C somewhere
    
    xEventGroupWaitBits(traceroute_events, TRACEROUTE_RUNNING | TRACEROUTE_COMPLETE, pdTRUE, pdTRUE, portMAX_DELAY);
    return 0;

exit:
    if (NULL != inst) {
        if (NULL != inst->hostname) {
            free(inst->hostname);
            inst->hostname = NULL;
        }

        free(inst);
    }
    xEventGroupClearBits(traceroute_events, TRACEROUTE_RUNNING);

    return 1;
}

void register_traceroute(void)
{
    traceroute_args.first_ttl = arg_int0("f", NULL, "<first_ttl>", "set the initial time-to-live used for the first outgoing probe packet");
    traceroute_args.m_first_ttl = arg_int0("M", NULL, "<first_ttl>", "set the initial time-to-live used for the first outgoing probe packet");
    traceroute_args.max_ttl = arg_int0("m", NULL, "<max_ttl>", "the maximum number of hops");
    traceroute_args.nqueries = arg_int0("q", NULL, "<nqueries>", "set the number of probes per hop");
    traceroute_args.tos = arg_int0("t", NULL, "<tos>", "the type-of-service in probe packets");
    traceroute_args.host = arg_str1(NULL, NULL, "<host>", "Host address");
    traceroute_args.end = arg_end(1);

    const esp_console_cmd_t traceroute_cmd = {
        .command = "traceroute",
        .help = "print the route packets take to network host",
        .hint = NULL,
        .func = &do_traceroute_cmd,
        .argtable = &traceroute_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&traceroute_cmd));

    traceroute_events = xEventGroupCreate();
}