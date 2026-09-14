/*
 * wifi_known -- remembered WiFi networks: the join that stores them, reconnection when the
 * link drops, and the rejoin at boot. See wifi_known.c.
 */
#pragma once

#include <stdbool.h>
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

/* `wifi on|off`: whether the station should be connected. Off keeps the stored networks. */
void wifi_known_set_enabled(bool enabled);
bool wifi_known_is_enabled(void);

#ifdef __cplusplus
}
#endif
