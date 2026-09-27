/*
 * wifi_ap -- the board's own access point, for when there is no network to join: phones and
 * laptops connect to it directly. On demand only (`wifi ap on`), never persisted: every boot
 * starts with it off. See wifi_ap.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool on;
    char ssid[33];
    char passphrase[65];
    uint32_t ip;                /* network byte order, as esp_ip4_addr_t */
    uint8_t channel;
    int clients;
} wifi_ap_info_t;

/*
 * Start the access point, alongside the station if it is up (APSTA). WPA2 with the stored
 * SSID and passphrase: by default `<CONFIG_CMD_WIFI_AP_SSID_PREFIX>-xxxx` (the end of the
 * station MAC) and a random passphrase made on first use and kept in NVS. Its DHCP server
 * hands out the board as DNS server and as the captive-portal URL, and a small DNS
 * responder answers every name with the board, so a phone that joins opens its page.
 */
esp_err_t wifi_ap_start(void);
esp_err_t wifi_ap_stop(void);
bool wifi_ap_is_on(void);
void wifi_ap_get_info(wifi_ap_info_t *out);

/* Store a new SSID and/or passphrase (NULL: unchanged). A running access point restarts with
 * them. ESP_ERR_INVALID_ARG for an empty or over-long SSID, or a passphrase outside 8..63. */
esp_err_t wifi_ap_set_credentials(const char *ssid, const char *passphrase);

/* The name the board goes by: `<prefix>-xxxx`, xxxx the last two bytes of the station MAC. */
void wifi_ap_default_name(const char *prefix, char *out, size_t out_len);

/* `wifi ap [on|off] [--ssid <ssid>] [--pass <passphrase>]`; argv[0] is "ap". Called by `wifi`. */
int wifi_ap_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
