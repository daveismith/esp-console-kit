#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include <stdint.h>
#include "esp_err.h"
#include "lwip/ip_addr.h"

/* Traceroute Task */
#ifndef ESP_TASK_TRACEROUTE_STACK
    #define ESP_TASK_TRACEROUTE_STACK             (2048 + TASK_EXTRA_STACK_SIZE)
#endif //ESP_TASK_TRACEROUTE_STACK

/**
 * @brief Type of "traceroute" session handle
 * 
 */
typedef void *esp_traceroute_handle_t;

/**
 * @brief Type of "traceroute" callback functions
 * 
 */
typedef struct {
    /**
     * @brief arguments for callback function
     * 
     */
    void *cb_args;

    /**
     * @brief Invoked by internal traceroute thread when a ... is received.
     *
     */
    void (*on_traceroute_success)(esp_traceroute_handle_t hdl, void *args);

    /**
     * @brief Invoked by internal traceroute thread when a ... is times out.
     * 
     */
    void (*on_traceroute_timeout)(esp_traceroute_handle_t hdl, void *args);

    /**
     * @brief Invoked by internal traceroute thread when a traceroute session is started.
     * 
     */
    void (*on_traceroute_start)(esp_traceroute_handle_t hdl, void *args);

    /**
     * @brief Invoked by internal traceroute thread when a traceroute session is finished.
     * 
     */
    void (*on_traceroute_end)(esp_traceroute_handle_t hdl, void *args);
} esp_traceroute_callbacks_t;

/**
 * @brief Type of "traceroute" configuration
 * 
 */
typedef struct {
    uint32_t interval_ms;       /*!< Milliseconds between each ping procedure */
    uint32_t timeout_ms;        /*!< Timeout value (in milliseconds) of each TTL procedure */
    uint32_t nqueries;          /*!< Number of queries per TTL */
    int tos;                    /*!< Type of Service, a field specified in the IP header */
    int first_ttl;              /*!< First Time to Live, the value to start counting from */
    int max_ttl;                /*!< Time to Live,a field specified in the IP header */

    ip_addr_t target_addr;      /*!< Target IP address, eitehr IPv4 or IPv6 */
    uint32_t task_stack_size;   /*!< Stack size of internal traceroute task */
    uint32_t task_prio;         /*!< Priority of internal traceroute task */
    uint32_t interface;         /*!< Netif index, interface=0 means NETIF_NO_INDEX */
} esp_traceroute_config_t;

/**
 * @brief Default traceroute configuration
 * 
 */
#define ESP_TRACEROUTE_DEFAULT_CONFIG()     \
    {                                       \
        .interval_ms = 1000,                \
        .timeout_ms = 5000,                 \
        .nqueries = 3,                      \
        .tos = 0,                           \
        .first_ttl = 1,                     \
        .max_ttl = IP_DEFAULT_TTL,          \
        .target_addr = *(IP_ANY_TYPE),      \
        .task_stack_size = ESP_TASK_TRACEROUTE_STACK,   \
        .task_prio = 2,                     \
        .interface = 0,                     \
    }

/**
 * @brief Profile of traceroute session
 */
typedef enum {
    ESP_TRACEROUTE_PROF_TTL,    /*!< Time ot live of the message */
    ESP_TRACEROUTE_PROF_IPADDR, /*!< IP address of replied target */
    ESP_TRACEROUTE_PROF_TIMEGAP, /*!< Elapsed time between request and reply packet in microseconds */
} esp_traceroute_profile_t;

/**
 * @brief Create a traceroute session
 *
 * @param config traceroute configuration
 * @param cbs a bunch of callback functions invoked by internal traceroute task
 * @param hdl_out handle of traceroute session
 * @return
 *      - ESP_ERR_INVALID_ARG: invalid parameters (e.g. configuration is null, etc)
 *      - ESP_ERR_NO_MEM: out of memory
 *      - ESP_FAIL: other internal error (e.g. socket error)
 *      - ESP_OK: create traceroute session successfully, user can take the traceroute handle to do follow-on jobs
 */
esp_err_t esp_traceroute_new_session(const esp_traceroute_config_t *config, const esp_traceroute_callbacks_t *cbs, esp_traceroute_handle_t *hdl_out);

/**
 * @brief Delete a traceroute session
 *
 * @param hdl handle of traceroute session
 * @return
 *      - ESP_ERR_INVALID_ARG: invalid parameters (e.g. ping handle is null, etc)
 *      - ESP_OK: delete traceroute session successfully
 */
esp_err_t esp_traceroute_delete_session(esp_traceroute_handle_t hdl);

/**
 * @brief Start the traceroute session
 *
 * @param hdl handle of traceroute session
 * @return
 *      - ESP_ERR_INVALID_ARG: invalid parameters (e.g. traceroute handle is null, etc)
 *      - ESP_OK: start traceroute session successfully
 */
esp_err_t esp_traceroute_start(esp_traceroute_handle_t hdl);

/**
 * @brief Stop the ping session
 *
 * @param hdl handle of ping session
 * @return
 *      - ESP_ERR_INVALID_ARG: invalid parameters (e.g. ping handle is null, etc)
 *      - ESP_OK: stop ping session successfully
 */
esp_err_t esp_traceroute_stop(esp_traceroute_handle_t hdl);

/**
 * @brief Get runtime profile of traceroute session
 *
 * @param hdl handle of traceroute session
 * @param profile type of profile
 * @param data profile data
 * @param size profile data size
 * @return
 *      - ESP_ERR_INVALID_ARG: invalid parameters (e.g. ping handle is null, etc)
 *      - ESP_ERR_INVALID_SIZE: the actual profile data size doesn't match the "size" parameter
 *      - ESP_OK: get profile successfully
 */
esp_err_t esp_traceroute_get_profile(esp_traceroute_handle_t hdl, esp_traceroute_profile_t profile, void *data, uint32_t size);

#ifdef __cplusplus
}
#endif