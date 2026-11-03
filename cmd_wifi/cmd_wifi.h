/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Console example — declarations of command registration functions.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once


#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"
#ifdef __cplusplus
extern "C" {
#endif

// Register WiFi functions
void register_wifi(void);

/*
 * Bring the radio up in WIFI_MODE_NULL with the netifs and the event loop in place, without
 * joining anything. Idempotent. Called by wifi_known_start() and by `join` on first use.
 */
void wifi_bringup(void);

/*
 * The radio's factory MAC for an interface (WIFI_IF_STA or WIFI_IF_AP), before any
 * override. On a chip with its own radio this is esp_read_mac(); on a host whose radio is
 * a co-processor (the ESP32-P4 through esp_hosted) the efuse has no WiFi MAC and
 * esp_read_mac() logs an error and fails, so the first value the co-processor reports
 * through esp_wifi_get_mac() is kept as the factory one. Needs esp_wifi_init() on a
 * remote radio; false if nothing can say.
 */
bool wifi_factory_mac(wifi_interface_t ifx, uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
