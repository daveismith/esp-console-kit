/*
 * web_net -- Wi-Fi over HTTP: the /api/v1/network routes, over wifi_known and wifi_ap. See
 * web_net.c.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the routes and list "network" as a feature. */
esp_err_t web_net_register(void);

#ifdef __cplusplus
}
#endif
