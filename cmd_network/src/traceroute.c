#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "traceroute.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "lwip/icmp.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"


const static char *TAG = "traceroute";

#define TRACEROUTE_TIME_DIFF_MS(_end, _start) ((uint32_t)(((_end).tv_sec - (_start).tv_sec) * 1000 + \
                                                    ((_end).tv_usec - (_start).tv_usec) / 1000))

#define TRACEROUTE_TIME_DIFF_US(_end, _start) ((uint64_t)(((_end).tv_sec - (_start).tv_sec) * 1000000 + \
                                                    ((_end).tv_usec - (_start).tv_usec)))


#define TRACEROUTE_CHECK_START_TIMEOUT_MS (1000)

#define TRACEROUTE_FLAGS_INIT (1 << 0)
#define TRACEROUTE_FLAGS_START (1 << 1)
#define TRACEROUTE_FLAGS_TERM (1 << 2)

typedef struct {
    int sock;
    int dgram_sock;
    struct sockaddr_storage target_addr;
    TaskHandle_t traceroute_task_hdl;

    ip_addr_t recv_addr;
    
    
    uint32_t interval_ms;
    uint32_t nqueries;
    
    int ttl;
    uint8_t first_ttl;
    uint8_t max_ttl;
    uint8_t tos;
    uint32_t flags;
    uint16_t port;
    uint64_t last_hop_us;

    void (*on_traceroute_success)(esp_traceroute_handle_t hdl, void *args);
    void (*on_traceroute_timeout)(esp_traceroute_handle_t hdl, void *args);
    void (*on_traceroute_start)(esp_traceroute_handle_t hdl, void *args);
    void (*on_traceroute_end)(esp_traceroute_handle_t hdl, void *args);
    void *cb_args;
} esp_traceroute_t;

#ifdef PACK_STRUCT_USE_INCLUDES
#  include "arch/bpstruct.h"
#endif
/** This is the standard ICMP header only that the u32_t data
 *  is split to two u16_t like ICMP echo needs it.
 *  This header is also used for other ICMP types that do not
 *  use the data part.
 */
PACK_STRUCT_BEGIN
struct icmp_ttl_hdr {
  PACK_STRUCT_FLD_8(u8_t type);
  PACK_STRUCT_FLD_8(u8_t code);
  PACK_STRUCT_FIELD(u16_t chksum);
  PACK_STRUCT_FIELD(u16_t id);
  PACK_STRUCT_FIELD(u16_t seqno);
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END
#ifdef PACK_STRUCT_USE_INCLUDES
#  include "arch/epstruct.h"
#endif

static esp_err_t esp_traceroute_send(esp_traceroute_t *tr)
{
    esp_err_t ret = ESP_OK;

    // Need To Set Port
    // Need to set destination port
    // need to set payload
    size_t len = 32;
    uint8_t *payload = mem_calloc(len, sizeof(uint8_t));
    if (NULL == payload) {
        ret = ESP_FAIL;
        goto err;
    }

    for (size_t idx = 0; idx < len; idx++) {
        payload[idx] = 0x40 + idx;
    }

    /* set ttl */
    setsockopt(tr->dgram_sock, IPPROTO_IP, IP_TTL, &tr->ttl, sizeof(tr->ttl));

    if (AF_INET == tr->target_addr.ss_family) {
        struct sockaddr_in *to4 = (struct sockaddr_in *)&tr->target_addr;
        to4->sin_port = htons(tr->port++);
    }
#if CONFIG_LWIP_IPV6
    else if (AF_INET6 == tr->target_addr.ss_family) {
        printf("ipv6\n");
    }
#endif

    //TODO: How to handle this IPv6?
    ssize_t sent = sendto(tr->dgram_sock, payload, len * sizeof(uint8_t), 0, 
                          (struct sockaddr *)&tr->target_addr, sizeof(tr->target_addr));

    if (sent != (ssize_t)(len * sizeof(uint8_t))) {
        ESP_LOGD(TAG, "sendto failed: sent %d of %u", (int)sent, (unsigned)len);
        ret = ESP_FAIL;
    }

err:
    if (payload != NULL) {
        mem_free(payload);
    }
    return ret;
}

// TODO: The Received
static int esp_traceroute_receive(esp_traceroute_t *tr)
{
    char buf[64]; // 64 bytes are enough to cover IP header and ICMP header
    int len = 0;
    struct sockaddr_storage from;
    ip_addr_t target_addr = { 0 };
    int fromlen = sizeof(from);
    uint16_t data_head = 0;

    while ((len = recvfrom(tr->sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, (socklen_t *)&fromlen)) > 0) {
        //lwip_sockaddr_to_ipaddr(&from_addr, (const struct sockaddr *)&from);
        if (AF_INET == from.ss_family) {
            // IPv4
            struct sockaddr_in *from4 = (struct sockaddr_in *)&from;
            inet_addr_to_ip4addr(ip_2_ip4(&tr->recv_addr), &from4->sin_addr);
            IP_SET_TYPE_VAL(tr->recv_addr, IPADDR_TYPE_V4);
            data_head = (uint16_t)(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr));

            // Copy Target
            const struct sockaddr_in *target4 = (const struct sockaddr_in *)&tr->target_addr;
            //ip4_addr_copy(target_addr.u_addr.ip4, *(struct ip4_addr *)&from4->sin_addr);
            inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &target4->sin_addr);
            IP_SET_TYPE_VAL(target_addr, IPADDR_TYPE_V4);
        }
#if CONFIG_LWIP_IPV6
        else {
            // IPv6
            struct sockaddr_in6 *from6 = (struct sockaddr_in6 *)&from;
            inet6_addr_to_ip6addr(ip_2_ip6(&tr->recv_addr), &from6->sin6_addr);
            IP_SET_TYPE_VAL(tr->recv_addr, IPADDR_TYPE_V6);
            data_head = (uint16_t)(sizeof(struct ip6_hdr) + sizeof(struct icmp6_echo_hdr));

            const struct sockaddr_in6 *target6 = (const struct sockaddr_in6 *)&tr->target_addr;
            inet6_addr_to_ip6addr(ip_2_ip6(&target_addr), &target6->sin6_addr);
            IP_SET_TYPE_VAL(target_addr, IPADDR_TYPE_V6);
        }
#endif

        // Check if the response is equal to the target address. If it is, then we should stop the
        // traceroute process after this loop.
        if (ip_addr_cmp(&tr->recv_addr, &target_addr)) {
            tr->flags |= TRACEROUTE_FLAGS_TERM; // Configure teh flags
        }

        if (len >= data_head) {
            if (IP_IS_V4_VAL(tr->recv_addr)) {              // Currently we process IPv4
                struct ip_hdr *iphdr = (struct ip_hdr *)buf;
                struct icmp_ttl_hdr *ittl = (struct icmp_ttl_hdr *)(buf + (IPH_HL(iphdr) * 4));
                if ((ICMP_TE == ittl->type)) {
                    // Got A Timeout (ICMP_TE) which means the TTL was exceeded
                    return len;
                } else if ((ICMP_DUR == ittl->type) && (3 == ittl->code)) {
                    // Got a Destination Unreachable (ICMP_DUR) which means the port doesn't exist and we're done
                    return len;
                }
            }
#if CONFIG_LWIP_IPV6
            else if (IP_IS_V6_VAL(tr->recv_addr)) {      // Currently we process IPv6
                const struct sockaddr_in6 *from_addr = (const struct sockaddr_in6 *)&from;
                struct icmp6_hdr *ittl6 = (struct icmp6_hdr *)(buf + sizeof(struct ip6_hdr)); // IPv6 head length is 40
                printf("got icmp6 of type %"PRIu8" from %s\n", ittl6->type, inet6_ntoa(from_addr->sin6_addr));
                //ICMP6_TYPE_TE
                if (ICMP6_TYPE_TE == ittl6->type) {
                    return len;
                } else if (ICMP6_TYPE_DUR == ittl6->type) {
                    return len;
                }
            }
#endif
        }
        fromlen = sizeof(from);
    }
    // if timeout, len will be -1
    return len;
}

static void esp_traceroute_thread(void *args)
{
    esp_traceroute_t *tr = (esp_traceroute_t *)(args);
    //TickType_t last_wake;
    struct timeval start_time, end_time;
    int recv_ret;

    while(1) {
        /* wait for traceroute start signal */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TRACEROUTE_CHECK_START_TIMEOUT_MS)))
        {
            size_t attempt = 0;

            tr->ttl = tr->first_ttl;
            tr->flags &= ~(TRACEROUTE_FLAGS_TERM);  // Clear The Term Flag
    
            if (tr->on_traceroute_start) {
                tr->on_traceroute_start((esp_traceroute_handle_t)tr, tr->cb_args);
            }

            while ((tr->flags & TRACEROUTE_FLAGS_START) && tr->ttl <= tr->max_ttl) {
                // Send TR?
                esp_traceroute_send(tr);
                gettimeofday(&start_time, NULL);
                recv_ret = esp_traceroute_receive(tr);
                gettimeofday(&end_time, NULL);
                uint32_t elapsed = TRACEROUTE_TIME_DIFF_MS(end_time, start_time);
                tr->last_hop_us = TRACEROUTE_TIME_DIFF_US(end_time, start_time);
                if (recv_ret >= 0) {
                    if (tr->on_traceroute_success) {
                        tr->on_traceroute_success((esp_traceroute_handle_t)tr, tr->cb_args);
                    }
                } else {
                    printf("timed out (%"PRIu32"ms)\n", elapsed);
                    if (tr->on_traceroute_timeout) {
                        tr->on_traceroute_timeout((esp_traceroute_handle_t)tr, tr->cb_args);
                    }
                }

                /*if (pdMS_TO_TICKS(tr->interval_ms)) {
                    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(tr->interval_ms));
                }*/
                
                if (++attempt >= tr->nqueries)
                {
                    if (tr->flags & TRACEROUTE_FLAGS_TERM) {
                        // Terminate
                        break;
                    } else {
                        attempt = 0;
                        tr->ttl += 1;                        
                    }
                }
            }
            /* One run per session: the caller builds a new session for every command and
             * never reuses this one. Leaving the loop here is what guarantees
             * on_traceroute_end fires even when the target was never reached. */
            break;
        } else {
            // check if traceroute has been de-initialized
            if (!(tr->flags & TRACEROUTE_FLAGS_INIT)) {
                break;
            } else if (tr->flags & TRACEROUTE_FLAGS_TERM) {
                break;
            }
        }
    }

    if (tr->on_traceroute_end) {
        tr->on_traceroute_end((esp_traceroute_handle_t)tr, tr->cb_args);
    }

    /* before exit task, free all resources */
    if (tr->sock > 0) {
        close(tr->sock);
    }
    if (tr->dgram_sock > 0) {
        close(tr->dgram_sock);
    }
    if (tr) {
        free(tr);
    }
    vTaskDelete(NULL);
}

esp_err_t esp_traceroute_new_session(const esp_traceroute_config_t *config, const esp_traceroute_callbacks_t *cbs, esp_traceroute_handle_t *hdl_out)
{
    esp_err_t ret = ESP_FAIL;
    esp_traceroute_t *tr = NULL;
    ESP_GOTO_ON_FALSE(config, ESP_ERR_INVALID_ARG, err, TAG, "traceroute config can't be null");
    ESP_GOTO_ON_FALSE(hdl_out, ESP_ERR_INVALID_ARG, err, TAG, "traceroute handle can't be null");

    tr = mem_calloc(1, sizeof(esp_traceroute_t));
    ESP_GOTO_ON_FALSE_ISR(tr, ESP_ERR_NO_MEM, err, TAG, "no memory for esp_traceroute_t object");

    /* set INIT flag, so that traceroute task won't exit (must set before create traceroute task) */
    tr->flags |= TRACEROUTE_FLAGS_INIT;

    /* create traceroute thread */
    BaseType_t xReturned = xTaskCreate(esp_traceroute_thread, "traceroute", config->task_stack_size, tr,
                                       config->task_prio, &tr->traceroute_task_hdl);
    ESP_GOTO_ON_FALSE(xReturned == pdTRUE, ESP_ERR_NO_MEM, err, TAG, "create traceroute task failed");

    /* callback functions */
    if (cbs) {
        tr->cb_args = cbs->cb_args;
        tr->on_traceroute_start = cbs->on_traceroute_start;
        tr->on_traceroute_end = cbs->on_traceroute_end;
        tr->on_traceroute_timeout = cbs->on_traceroute_timeout;
        tr->on_traceroute_success = cbs->on_traceroute_success;
    }

    /* set parameters for traceroute */
    tr->first_ttl = config->first_ttl;
    tr->interval_ms = config->interval_ms;
    tr->nqueries = config->nqueries;

    /* create socket */
    if (IP_IS_V4(&config->target_addr)
#if CONFIG_LWIP_IPV6
        || ip6_addr_isipv4mappedipv6(ip_2_ip6(&config->target_addr))
#endif
    ) {
        tr->sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
        tr->dgram_sock = socket(AF_INET, SOCK_DGRAM, IP_PROTO_UDP);
    }
#if CONFIG_LWIP_IPV6
    else {
        tr->sock = socket(AF_INET6, SOCK_RAW, IP6_NEXTH_ICMP6);
        tr->dgram_sock = socket(AF_INET6, SOCK_DGRAM, IP_PROTO_UDP);
    }
#endif
    ESP_GOTO_ON_FALSE(tr->sock >= 0, ESP_FAIL, err, TAG, "create socket failed: %d", tr->sock);
    ESP_GOTO_ON_FALSE(tr->dgram_sock >= 0, ESP_FAIL, err, TAG, "create datagram socket failed: %d", tr->dgram_sock);

    /* set if index */
    if(config->interface) {
        struct ifreq iface;
        if(netif_index_to_name(config->interface, iface.ifr_name) == NULL) {
            ESP_LOGE(TAG, "fail to find interface name with netif index %"PRIu32, config->interface);
            goto err;
        } 
        if(setsockopt(tr->sock, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) != 0) {
            ESP_LOGE(TAG, "fail to setsockopt SO_BINDTODEVICE on socket");
            goto err;
        }
        if(setsockopt(tr->dgram_sock, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) != 0) {
            ESP_LOGE(TAG, "fail to setsockopt SO_BINDTODEVICE on dgram_socket");
            goto err;
        }
    }
    struct timeval timeout;
    timeout.tv_sec = config->timeout_ms / 1000;
    timeout.tv_usec = (config->timeout_ms % 1000) * 1000;
    /* set receive timeout */
    setsockopt(tr->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* set params timeout */
    tr->max_ttl = (uint8_t)config->max_ttl;

    /* set tos */
    setsockopt(tr->sock, IPPROTO_IP, IP_TOS, &config->tos, sizeof(config->tos));
    setsockopt(tr->dgram_sock, IPPROTO_IP, IP_TOS, &config->tos, sizeof(config->tos));

    /* set ttl (we'll mess with this over time) */

    tr->port = 33434;

    /* set socket address */
    if (IP_IS_V4(&config->target_addr)) {
        struct sockaddr_in *to4 = (struct sockaddr_in *)&tr->target_addr;
        to4->sin_family = AF_INET;
        to4->sin_port = htons(tr->port); // One Lower Because We Increment Above
        inet_addr_from_ip4addr(&to4->sin_addr, ip_2_ip4(&config->target_addr));;
        // TODO: Packet Set Type
    }
#if CONFIG_LWIP_IPV6
    if (IP_IS_V6(&config->target_addr)) {
        struct sockaddr_in6 *to6 = (struct sockaddr_in6 *)&tr->target_addr;
        to6->sin6_family = AF_INET6;
        to6->sin6_port = htons(tr->port);
        inet6_addr_from_ip6addr(&to6->sin6_addr, ip_2_ip6(&config->target_addr));
        // TODO: Packet Set Type
    }
#endif
    /* return traceroute handle to user */
    *hdl_out = (esp_traceroute_handle_t)tr;
    return ESP_OK;
err:
    if (tr) {
        if (tr->sock > 0) {
            close(tr->sock);
        }
        if (tr->dgram_sock > 0) {
            close(tr->dgram_sock);
        }
        if (tr->traceroute_task_hdl) {
            vTaskDelete(tr->traceroute_task_hdl);
        }
        free(tr);
    }
    return ret;
}

esp_err_t esp_traceroute_delete_session(esp_traceroute_handle_t hdl)
{
    esp_err_t ret = ESP_OK;
    esp_traceroute_t *tr = (esp_traceroute_t *)hdl;
    ESP_GOTO_ON_FALSE(tr, ESP_ERR_INVALID_ARG, err, TAG, "traceroute handle can't be null");
    /* reset init flags, then traceroute task will exit */
    tr->flags &= ~TRACEROUTE_FLAGS_INIT;
    return ESP_OK;
err:
    return ret;
}

esp_err_t esp_traceroute_start(esp_traceroute_handle_t hdl)
{
    esp_err_t ret = ESP_OK;
    esp_traceroute_t *tr = (esp_traceroute_t *)hdl;
    ESP_GOTO_ON_FALSE(tr, ESP_ERR_INVALID_ARG, err, TAG, "traceroute handle can't be null");
    tr->flags |= TRACEROUTE_FLAGS_START;
    xTaskNotifyGive(tr->traceroute_task_hdl);
    return ESP_OK;
err:
    return ret;
}

esp_err_t esp_traceroute_stop(esp_traceroute_handle_t hdl)
{
    esp_err_t ret = ESP_OK;
    esp_traceroute_t *tr = (esp_traceroute_t *)hdl;
    ESP_GOTO_ON_FALSE(tr, ESP_ERR_INVALID_ARG, err, TAG, "traceroute handle can't be null");
    tr->flags &= ~TRACEROUTE_FLAGS_START;
    return ESP_OK;
err:
    return ret;
}

esp_err_t esp_traceroute_get_profile(esp_traceroute_handle_t hdl, esp_traceroute_profile_t profile, void *data, uint32_t size)
{
    esp_err_t ret = ESP_OK;
    esp_traceroute_t *tr = (esp_traceroute_t *)hdl;
    const void *from = NULL;
    uint32_t copy_size = 0;
    ESP_GOTO_ON_FALSE(tr, ESP_ERR_INVALID_ARG, err, TAG, "traceroute handle can't be null");
    ESP_GOTO_ON_FALSE(data, ESP_ERR_INVALID_ARG, err, TAG, "profile data can't be null");

    switch (profile) {
        case ESP_TRACEROUTE_PROF_TTL:
            from = &tr->ttl;
            copy_size = sizeof(tr->ttl);
            break;
        case ESP_TRACEROUTE_PROF_IPADDR:
            from = &tr->recv_addr;
            copy_size = sizeof(tr->recv_addr);
            break;
        case ESP_TRACEROUTE_PROF_TIMEGAP:
            from = &tr->last_hop_us;
            copy_size = sizeof(tr->last_hop_us);
            break;
        default:
            ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown profile: %d", profile);
            break;
    }
    ESP_GOTO_ON_FALSE(size >= copy_size, ESP_ERR_INVALID_SIZE, err, TAG, "unmatched data size for profile %d", profile);
    memcpy(data, from, copy_size);
    return ESP_OK;
err:
    return ret;
}
