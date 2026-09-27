/*
 * wifi_known -- remembered WiFi networks: the join that stores them, reconnection when the
 * link drops, and the rejoin at boot. See wifi_known.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_KNOWN_EVT_JOINING,       /* a join has been started */
    WIFI_KNOWN_EVT_GOT_IP,        /* the station has an IPv4 address */
    WIFI_KNOWN_EVT_JOIN_FAILED,   /* a join failed for a reason other than authentication */
    WIFI_KNOWN_EVT_AUTH_FAILED,   /* a join failed authentication; nothing was stored */
    WIFI_KNOWN_EVT_LINK_LOST,     /* a link that had an address dropped */
} wifi_known_event_t;

typedef struct {
    wifi_known_event_t event;
    const char *ssid;             /* valid only for the duration of the hook call */
    int reason;                   /* wifi_err_reason_t for failures and losses, else 0 */
    bool reconnecting;            /* whether a reconnect will be attempted */
} wifi_known_info_t;

/* Called on the default event loop task. Must not block. */
typedef void (*wifi_known_hook_t)(const wifi_known_info_t *info, void *ctx);

/* Register `wifi [on|off]`, `wifi_save`, `wifi_forget` and `wifi_known`. */
void wifi_known_register_commands(void);

/*
 * Bring the radio up in station mode (via wifi_bringup()), install the reconnect policy and
 * rejoin the network last joined successfully, if there is one. Idempotent. Needs NVS and
 * the default event loop.
 */
esp_err_t wifi_known_start(void);

/* One observer for link changes -- a UI, a log, a server that binds per interface. */
void wifi_known_set_hook(wifi_known_hook_t hook, void *ctx);

/*
 * Join `ssid` and remember it. A NULL or empty passphrase uses the stored one for a known
 * network (or joins an open one). Returns once the join has been started; the outcome
 * arrives through the hook.
 */
esp_err_t wifi_known_join(const char *ssid, const char *passphrase);

/* Drop a stored credential, and the link if it is the one in use. */
esp_err_t wifi_known_forget(const char *ssid, bool *was_stored);

/* Each stored network: its SSID, whether it has a passphrase (never what it is), and whether it
 * is the one rejoined at boot. Under the store's lock: `cb` must not block. False stops. */
typedef bool (*wifi_known_list_cb_t)(const char *ssid, bool has_passphrase, bool last, void *ctx);
void wifi_known_list(wifi_known_list_cb_t cb, void *ctx);
bool wifi_known_has(const char *ssid);

/* Store a network without joining it (a NULL or empty passphrase for an open one): it is
 * joined when wifi_known_join() names it. The store keeps 16; a 17th forgets the oldest.
 * ESP_ERR_INVALID_ARG for an SSID of 0 or over 32 bytes, or a passphrase outside 8..63. */
esp_err_t wifi_known_save(const char *ssid, const char *passphrase);

/* The networks in range, one entry an SSID (its strongest), strongest first; hidden ones
 * left out. Blocks for the scan, about 2-3 s. ESP_ERR_WIFI_STATE while the station is
 * connecting. */
typedef struct {
    char ssid[33];
    int rssi;
    unsigned channel;
    const char *auth;           /* "open", "wep", "wpa", "wpa2", "wpa3", "enterprise", "other" */
    bool known;
} wifi_known_scan_t;
esp_err_t wifi_known_scan(wifi_known_scan_t *out, size_t max, size_t *found);

/* `wifi on|off`: whether the station should be connected. Off keeps the stored networks. */
void wifi_known_set_enabled(bool enabled);
bool wifi_known_is_enabled(void);

#ifdef __cplusplus
}
#endif
