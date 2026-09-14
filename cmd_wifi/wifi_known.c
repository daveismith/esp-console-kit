/*
 * wifi_known -- remembered WiFi networks: the join that stores them, reconnection when the
 * link drops, and the rejoin at boot.
 *
 * Extracted from r2_domeplayer's main/panel_net.c. What stayed there is UI: the scan
 * provider, static addressing, the MAC override and the access point. What came across is
 * the part that had bugs worth not having twice -- see the comments in the event handler.
 *
 * One deliberate change from panel_net.c: a new credential is held in RAM while its join is
 * in flight and committed to the store only when the join succeeds, or fails for a reason
 * other than authentication (out of range says nothing about the passphrase, and keeping it
 * lets a network be saved before it is in range). A join that fails authentication leaves
 * the store exactly as it was, so a typo never replaces a passphrase that worked, and a
 * rejoin that fails authentication never erases one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cmd_wifi.h"
#include "wifi_known.h"

static const char *TAG = "wifi_known";

/*
 * KNOWN NETWORKS. One NVS blob, not a key per SSID: NVS keys are capped at 15 characters
 * (NVS_KEY_NAME_MAX_SIZE), and an SSID is up to 32 bytes -- so keying by SSID silently fails
 * to store more than half the networks in range. A blob also caps how much the store can
 * grow and makes "which was last joined" part of the same atomic write.
 *
 * The sizes are r2_domeplayer's, so its blob reads back unchanged with the namespace set to
 * "r2net". They must not change without a KNOWN_VERSION bump: the blob's size is part of
 * what known_load() validates.
 */
#define SSID_LEN      33   /* 32 + NUL, the 802.11 maximum */
#define PASS_LEN      64   /* 63 + NUL, the WPA2 maximum */
#define KNOWN_MAX     16
#define KNOWN_KEY     "known"
#define KNOWN_VERSION 1u

typedef struct {
    char ssid[SSID_LEN];
    char passphrase[PASS_LEN];
} known_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    char last[SSID_LEN];   /* rejoined at boot */
    known_entry_t entries[KNOWN_MAX];
} known_store_t;

/*
 * Everything below is shared between the console task (the commands) and the event task
 * (the handler), and guarded by s_lock. NULL until the first join or wifi_known_start():
 * before that nothing else is running, so there is nothing to serialise against.
 */
static SemaphoreHandle_t s_lock;
static known_store_t s_known;
static bool s_known_loaded;

static bool s_started;
static bool s_joining;
static char s_join_ssid[SSID_LEN];
static char s_join_pass[PASS_LEN];   /* held until the join resolves */
static bool s_join_commit;           /* s_join_pass is new and wants storing */
static bool s_have_ip;

/* The outcome of the last join, for `wifi_save` to wait on. */
typedef enum { RESULT_PENDING, RESULT_OK, RESULT_AUTH_FAILED, RESULT_FAILED } join_result_t;
static volatile join_result_t s_join_result = RESULT_PENDING;

/*
 * Reconnect policy. A disconnect while a join is in flight is a failure to report; a
 * disconnect after we had an address is a link to retry -- a few times at once, then every
 * CONFIG_CMD_WIFI_KNOWN_RETRY_INTERVAL_S, so a board whose access point went away for a
 * minute comes back by itself.
 */
static bool s_want_connected;
static int s_retries;
static esp_timer_handle_t s_retry_timer;

static wifi_known_hook_t s_hook;
static void *s_hook_ctx;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ the store */

/* The known_* helpers are called with the lock held. */
static void known_load(void)
{
    if (s_known_loaded) {
        return;
    }
    s_known_loaded = true;
    memset(&s_known, 0, sizeof(s_known));
    s_known.version = KNOWN_VERSION;

    nvs_handle_t h;
    if (nvs_open(CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_known);
    known_store_t *tmp = calloc(1, sizeof(*tmp));
    if (tmp != NULL) {
        if (nvs_get_blob(h, KNOWN_KEY, tmp, &len) == ESP_OK &&
            len == sizeof(*tmp) && tmp->version == KNOWN_VERSION &&
            tmp->count <= KNOWN_MAX) {
            memcpy(&s_known, tmp, sizeof(s_known));
        }
        free(tmp);
    }
    nvs_close(h);
}

static void known_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, KNOWN_KEY, &s_known, sizeof(s_known));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving networks: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

static known_entry_t *known_find(const char *ssid)
{
    known_load();
    for (uint32_t i = 0; i < s_known.count; i++) {
        if (strcmp(s_known.entries[i].ssid, ssid) == 0) {
            return &s_known.entries[i];
        }
    }
    return NULL;
}

static bool known_get(const char *ssid, char *out, size_t out_len)
{
    const known_entry_t *e = known_find(ssid);
    if (e == NULL) {
        return false;
    }
    strlcpy(out, e->passphrase, out_len);
    return true;
}

/* Store a credential and mark it the network to rejoin at boot, in one write. */
static void known_put(const char *ssid, const char *passphrase, bool as_last)
{
    known_load();
    known_entry_t *e = known_find(ssid);
    if (e == NULL) {
        if (s_known.count >= KNOWN_MAX) {
            /* Oldest out. A board that refuses to remember a new network because it is full
             * is worse than one that forgets the network you last used a year ago. */
            memmove(&s_known.entries[0], &s_known.entries[1],
                    sizeof(s_known.entries[0]) * (KNOWN_MAX - 1));
            s_known.count = KNOWN_MAX - 1;
        }
        e = &s_known.entries[s_known.count++];
        memset(e, 0, sizeof(*e));
        strlcpy(e->ssid, ssid, sizeof(e->ssid));
    }
    if (passphrase != NULL) {
        strlcpy(e->passphrase, passphrase, sizeof(e->passphrase));
    }
    if (as_last) {
        strlcpy(s_known.last, ssid, sizeof(s_known.last));
    }
    known_save();
}

static void known_put_last(const char *ssid)
{
    known_load();
    if (strcmp(s_known.last, ssid) == 0) {
        return;   /* every successful rejoin would otherwise be a flash write */
    }
    strlcpy(s_known.last, ssid, sizeof(s_known.last));
    known_save();
}

static void known_erase(const char *ssid)
{
    known_load();
    for (uint32_t i = 0; i < s_known.count; i++) {
        if (strcmp(s_known.entries[i].ssid, ssid) != 0) {
            continue;
        }
        memmove(&s_known.entries[i], &s_known.entries[i + 1],
                sizeof(s_known.entries[0]) * (s_known.count - i - 1));
        s_known.count--;
        break;
    }
    if (strcmp(s_known.last, ssid) == 0) {
        s_known.last[0] = '\0';
    }
    known_save();
}

/* ------------------------------------------------------------------ events */

static void emit(wifi_known_event_t event, const char *ssid, int reason, bool reconnecting)
{
    wifi_known_hook_t hook = s_hook;
    if (hook != NULL) {
        const wifi_known_info_t info = {
            .event = event, .ssid = ssid, .reason = reason, .reconnecting = reconnecting,
        };
        hook(&info, s_hook_ctx);
    }
}

static void schedule_retry(void)
{
#if CONFIG_CMD_WIFI_KNOWN_RETRY_INTERVAL_S > 0
    if (s_retry_timer != NULL && !esp_timer_is_active(s_retry_timer)) {
        esp_timer_start_once(s_retry_timer,
                             (uint64_t)CONFIG_CMD_WIFI_KNOWN_RETRY_INTERVAL_S * 1000000ULL);
    }
#endif
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    lock();
    const bool want = s_want_connected && !s_have_ip;
    unlock();
    if (want) {
        ESP_LOGI(TAG, "retrying");
        esp_wifi_connect();
    }
}

/*
 * The reasons the radio gives for "your passphrase is wrong" are several and none of them
 * says so. These are the ones that mean authentication, as opposed to a network that is
 * simply not there. NO_AP_FOUND is deliberately NOT among them: out of range is not a reason
 * to believe the passphrase is wrong.
 */
static bool is_auth_failure(int reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_MIC_FAILURE;
}

static void on_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    char from[SSID_LEN];
    const size_t len = MIN((size_t)ev->ssid_len, sizeof(from) - 1);
    memcpy(from, ev->ssid, len);
    from[len] = '\0';

    lock();
    /* Captured before it is cleared: a disconnect that HAD an address is a link that
     * dropped, and a disconnect that did not is a join or a reconnect that never arrived. */
    const bool had_ip = s_have_ip;
    s_have_ip = false;

    if (s_joining) {
        /*
         * NOT EVERY DISCONNECT DURING A JOIN IS THE JOIN FAILING. wifi_known_join() drops the
         * previous link on its way in, and that teardown arrives here as an ordinary
         * WIFI_EVENT_STA_DISCONNECTED -- reason 8, ASSOC_LEAVE, naming the network we just
         * left. Treating it as this join's failure was a real bug in panel_net.c: the join
         * then succeeded while the failure had already been reported and acted on. The event
         * names the AP it refers to, so ask it: a disconnect from anything other than the
         * network being joined must not touch the join state or trigger a reconnect.
         */
        if ((from[0] != '\0' && strcmp(from, s_join_ssid) != 0) ||
            ev->reason == WIFI_REASON_ASSOC_LEAVE) {
            unlock();
            ESP_LOGD(TAG, "ignoring disconnect from %s during join to %s", from, s_join_ssid);
            return;
        }
        const bool auth = is_auth_failure(ev->reason);
        char ssid[SSID_LEN];
        strlcpy(ssid, s_join_ssid, sizeof(ssid));
        s_joining = false;
        if (auth) {
            /* Retrying a wrong passphrase only fails again, and some access points lock a
             * client out for it. Nothing is stored; see the note at the top. */
            s_want_connected = false;
        } else if (s_join_commit) {
            known_put(ssid, s_join_pass, true);
        }
        s_join_commit = false;
        memset(s_join_pass, 0, sizeof(s_join_pass));
        const bool keep_trying = s_want_connected;
        s_join_result = auth ? RESULT_AUTH_FAILED : RESULT_FAILED;
        unlock();

        ESP_LOGW(TAG, "join %s failed, reason %d%s", ssid, (int)ev->reason,
                 auth ? " (authentication)" : "");
        emit(auth ? WIFI_KNOWN_EVT_AUTH_FAILED : WIFI_KNOWN_EVT_JOIN_FAILED, ssid,
             ev->reason, keep_trying);
        if (keep_trying) {
            schedule_retry();
        }
        return;
    }

    bool retry_now = false;
    if (s_want_connected && s_retries < CONFIG_CMD_WIFI_KNOWN_FAST_RETRIES) {
        s_retries++;
        retry_now = true;
    }
    const bool want = s_want_connected;
    unlock();

    if (had_ip) {
        ESP_LOGW(TAG, "link to %s lost, reason %d%s", from, (int)ev->reason,
                 want ? "; reconnecting" : "");
        emit(WIFI_KNOWN_EVT_LINK_LOST, from, ev->reason, want);
    }
    if (retry_now) {
        esp_wifi_connect();
    } else if (want) {
        schedule_retry();
    }
}

static void on_got_ip(void)
{
    lock();
    s_have_ip = true;
    s_retries = 0;
    if (s_joining) {
        s_joining = false;
        /* The passphrase is only rewritten when this join brought a new one. Rewriting it
         * from here unconditionally is how panel_net.c once replaced a stored passphrase
         * with an empty string the moment a join succeeded. */
        if (s_join_commit) {
            known_put(s_join_ssid, s_join_pass, true);
        } else {
            known_put_last(s_join_ssid);
        }
        s_join_commit = false;
        memset(s_join_pass, 0, sizeof(s_join_pass));
        s_join_result = RESULT_OK;
    }
    unlock();
    if (s_retry_timer != NULL) {
        esp_timer_stop(s_retry_timer);
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#if CONFIG_LWIP_IPV6
    /* lwIP does not bring a v6 link-local up by itself on esp_netif. A global address
     * follows by SLAAC if the router advertises a prefix. */
    if (sta != NULL) {
        esp_netif_create_ip6_linklocal(sta);
    }
#endif
    char ssid[SSID_LEN] = "";
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ssid, (const char *)ap.ssid, sizeof(ssid));
    }
    esp_netif_ip_info_t ip = { 0 };
    if (sta != NULL) {
        esp_netif_get_ip_info(sta, &ip);
    }
    ESP_LOGI(TAG, "joined %s, " IPSTR, ssid, IP2STR(&ip.ip));
    emit(WIFI_KNOWN_EVT_GOT_IP, ssid, 0, false);
}

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        on_disconnected(data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip();
    }
}

/* ------------------------------------------------------------------ the API */

/* Everything wifi_known_start() does except the rejoin, so a join can start it lazily. */
static esp_err_t known_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    wifi_bringup();

    SemaphoreHandle_t lock_handle = xSemaphoreCreateMutex();
    if (lock_handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock_handle);
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_cb, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* Station mode from the start: WIFI_MODE_NULL can neither scan nor join. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    s_lock = lock_handle;
    s_started = true;
    return ESP_OK;
}

esp_err_t wifi_known_start(void)
{
    const bool first = !s_started;
    esp_err_t err = known_init();
    if (err != ESP_OK || !first) {
        return err;
    }
    /* Rejoin whatever was last joined successfully. esp_wifi's own storage is RAM-only
     * (wifi_bringup() sets WIFI_STORAGE_RAM) -- this store decides what is remembered, not
     * the driver -- so this is the only thing that makes the link survive a power cycle. */
    char last[SSID_LEN];
    lock();
    known_load();
    strlcpy(last, s_known.last, sizeof(last));
    unlock();
    if (last[0] != '\0') {
        ESP_LOGI(TAG, "rejoining %s", last);
        return wifi_known_join(last, NULL);
    }
    ESP_LOGI(TAG, "no saved network; `wifi_save <ssid> [pass]` to add one");
    return ESP_OK;
}

void wifi_known_set_hook(wifi_known_hook_t hook, void *ctx)
{
    s_hook_ctx = ctx;
    s_hook = hook;
}

esp_err_t wifi_known_join(const char *ssid, const char *passphrase)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= SSID_LEN ||
        (passphrase != NULL && strlen(passphrase) >= PASS_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = known_init();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));

    lock();
    char stored[PASS_LEN];
    bool recalled = false;
    if ((passphrase == NULL || passphrase[0] == '\0') &&
        known_get(ssid, stored, sizeof(stored))) {
        passphrase = stored;   /* a known network is named, not retyped */
        recalled = true;
    }
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    strlcpy(s_join_pass, passphrase ? passphrase : "", sizeof(s_join_pass));
    strlcpy((char *)cfg.sta.password, s_join_pass, sizeof(cfg.sta.password));
    s_join_commit = !recalled;
    /*
     * The join is in flight from HERE, before the old link is dropped -- not after the
     * config is set. The teardown below raises a DISCONNECTED event, and the handler needs
     * the join state and the target SSID already in place to recognise it as ours. With the
     * assignment after the disconnect, panel_net.c had a window in which that event arrived
     * looking like a dropped link, and it reconnected to the OLD network moments before the
     * new configuration was set.
     */
    s_joining = true;
    s_join_result = RESULT_PENDING;
    s_want_connected = true;
    s_retries = 0;
    unlock();
    memset(stored, 0, sizeof(stored));

    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(mode == WIFI_MODE_AP ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    }
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));

    emit(WIFI_KNOWN_EVT_JOINING, ssid, 0, false);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        lock();
        s_joining = false;
        s_join_commit = false;
        memset(s_join_pass, 0, sizeof(s_join_pass));
        s_join_result = RESULT_FAILED;
        unlock();
        emit(WIFI_KNOWN_EVT_JOIN_FAILED, ssid, 0, false);
    }
    return err;
}

/* True when `ssid` is the network the radio is actually associated with right now. */
static bool sta_is_on(const char *ssid)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    char current[SSID_LEN];
    strlcpy(current, (const char *)ap.ssid, sizeof(current));
    return strcmp(current, ssid) == 0;
}

esp_err_t wifi_known_forget(const char *ssid, bool *was_stored)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const bool had = (known_find(ssid) != NULL);
    if (had) {
        known_erase(ssid);
    }
    const bool joining = s_joining && strcmp(s_join_ssid, ssid) == 0;
    unlock();

    /* Asked of the driver, not of s_join_ssid: a link made by the plain `join` command, or
     * one still up from the boot rejoin, is not in the join fields but is still in use. */
    if (s_started && (joining || sta_is_on(ssid))) {
        /* Before the disconnect, always: the DISCONNECTED handler reconnects while
         * s_want_connected is set, which would put the link straight back up. And the join
         * in flight is cleared, or a GOT_IP already on its way would write the SSID back as
         * the network to rejoin at boot -- the one thing forgetting it must prevent. */
        lock();
        s_want_connected = false;
        s_joining = false;
        s_join_commit = false;
        s_join_ssid[0] = '\0';
        memset(s_join_pass, 0, sizeof(s_join_pass));
        unlock();
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        /* The driver keeps its own RAM copy of the credential, and esp_wifi_connect() uses
         * that, not the store: without this `wifi on` would reconnect to a network we no
         * longer have a record of. */
        wifi_config_t cfg = { 0 };
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (was_stored != NULL) {
        *was_stored = had;
    }
    return ESP_OK;
}

void wifi_known_set_enabled(bool enabled)
{
    if (known_init() != ESP_OK) {
        return;
    }
    char last[SSID_LEN];
    lock();
    s_want_connected = enabled;
    if (enabled) {
        /* A fresh set of fast retries, as a join has. */
        s_retries = 0;
    } else {
        /* A join in flight is abandoned; its credential was never committed. */
        s_joining = false;
        s_join_commit = false;
        memset(s_join_pass, 0, sizeof(s_join_pass));
    }
    known_load();
    strlcpy(last, s_known.last, sizeof(last));
    unlock();

    if (!enabled) {
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        return;
    }
    wifi_config_t cfg = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
        esp_wifi_connect();
    } else if (last[0] != '\0') {
        wifi_known_join(last, NULL);
    }
}

bool wifi_known_is_enabled(void)
{
    return s_want_connected;
}

/* ------------------------------------------------------------------ console */

static bool route_on_wifi(void)
{
    esp_netif_t *n = esp_netif_get_default_netif();
    return n != NULL && strcmp(esp_netif_get_ifkey(n), "WIFI_STA_DEF") == 0 &&
           esp_netif_is_netif_up(n);
}

/* The link, and which interface carries the default route and whose DNS server answers. */
static void print_wifi_state(void)
{
    printf("wifi: %s\n", s_want_connected ? "on" : "off, until `wifi on` or a reboot");
    /* Only ask the driver about the access point with the station up: disconnected, the
     * question itself logs a warning. */
    wifi_ap_record_t ap;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != NULL && esp_netif_is_netif_up(sta) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_get_ip_info(sta, &ip);
        printf("link: %s, channel %u, %d dBm, " IPSTR "\n", (const char *)ap.ssid,
               ap.primary, ap.rssi, IP2STR(&ip.ip));
    } else {
        printf("link: not associated\n");
    }
    esp_netif_t *n = esp_netif_get_default_netif();
    char dns[20] = "none";
    esp_netif_dns_info_t d;
    if (n != NULL && esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &d) == ESP_OK &&
        d.ip.u_addr.ip4.addr != 0) {
        snprintf(dns, sizeof(dns), IPSTR, IP2STR(&d.ip.u_addr.ip4));
    }
    printf("route: default via %s, dns %s\n", n != NULL ? esp_netif_get_ifkey(n) : "nothing", dns);
}

/*
 * `wifi off` disconnects the station and nothing reconnects it; the stored networks are
 * untouched, and a reboot rejoins as usual. `wifi on` rejoins the network it was on. Either
 * waits for the default route to move before it prints, a moment down and a few seconds up.
 */
static int cmd_wifi_power(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("usage: wifi [on|off]\n");
        return 1;
    }
    if (argc == 2) {
        const bool on = strcmp(argv[1], "on") == 0;
        if (on == s_want_connected && route_on_wifi() == on) {
            /* Nothing to do; and reconnecting would drop a working link. */
            print_wifi_state();
            return 0;
        }
        wifi_known_set_enabled(on);
        const int limit_ms = on ? 15000 : 3000;
        for (int t = 0; t < limit_ms && route_on_wifi() != on; t += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    print_wifi_state();
    return 0;
}

static struct {
    struct arg_int *timeout;
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_wifi_save_args;

static int cmd_wifi_save(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_wifi_save_args) != 0) {
        arg_print_errors(stderr, s_wifi_save_args.end, argv[0]);
        return 1;
    }
    const char *ssid = s_wifi_save_args.ssid->sval[0];
    const char *pass = s_wifi_save_args.pass->count ? s_wifi_save_args.pass->sval[0] : NULL;
    const int timeout_ms = s_wifi_save_args.timeout->count ? s_wifi_save_args.timeout->ival[0]
                                                          : 15000;
    esp_err_t err = wifi_known_join(ssid, pass);
    if (err != ESP_OK) {
        printf("wifi_save: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("joining %s...\n", ssid);
    for (int t = 0; t < timeout_ms && s_join_result == RESULT_PENDING; t += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    switch (s_join_result) {
    case RESULT_OK:
        print_wifi_state();
        printf("saved; rejoined at boot\n");
        return 0;
    case RESULT_AUTH_FAILED:
        printf("authentication failed; nothing saved\n");
        return 1;
    case RESULT_FAILED:
        printf("could not join (out of range?); saved anyway, retrying in the background\n");
        return 1;
    default:
        printf("still joining after %d ms; it is saved once the join resolves\n", timeout_ms);
        return 1;
    }
}

static struct {
    struct arg_str *ssid;
    struct arg_end *end;
} s_wifi_forget_args;

static int cmd_wifi_forget(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_wifi_forget_args) != 0) {
        arg_print_errors(stderr, s_wifi_forget_args.end, argv[0]);
        return 1;
    }
    const char *ssid = s_wifi_forget_args.ssid->sval[0];
    bool had = false;
    wifi_known_forget(ssid, &had);
    printf(had ? "forgot %s\n" : "%s was not stored\n", ssid);
    return 0;
}

static int cmd_wifi_known(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lock();
    known_load();
    if (s_known.count == 0) {
        printf("no saved networks\n");
    }
    for (uint32_t i = 0; i < s_known.count; i++) {
        const known_entry_t *e = &s_known.entries[i];
        /*
         * Whether there is a passphrase, never what it is: the console is unauthenticated.
         * And "KEY"/"NO KEY" rather than "WPA"/"OPEN", because the store holds no authmode --
         * an entry saved with an empty passphrase for a WPA3 network would otherwise be
         * reported as an open one.
         */
        printf("%-32s %-8s%s\n", e->ssid, e->passphrase[0] ? "KEY" : "NO KEY",
               strcmp(s_known.last, e->ssid) == 0 ? "  <- rejoined at boot" : "");
    }
    unlock();
    return 0;
}

void wifi_known_register_commands(void)
{
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "WiFi on or off, the stored networks untouched; alone, the link and the default route",
        .hint = "[on|off]",
        .func = cmd_wifi_power,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    s_wifi_save_args.timeout = arg_int0(NULL, "timeout", "<ms>", "how long to wait for the result (default 15000)");
    s_wifi_save_args.ssid = arg_str1(NULL, NULL, "<ssid>", "network to join and remember");
    s_wifi_save_args.pass = arg_str0(NULL, NULL, "[pass]", "passphrase; omit for a known or open network");
    s_wifi_save_args.end = arg_end(3);
    const esp_console_cmd_t save_cmd = {
        .command = "wifi_save",
        .help = "Join a WiFi network and remember it; the last one joined is rejoined at boot",
        .hint = NULL,
        .func = cmd_wifi_save,
        .argtable = &s_wifi_save_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&save_cmd));

    s_wifi_forget_args.ssid = arg_str1(NULL, NULL, "<ssid>", "network to drop from the store");
    s_wifi_forget_args.end = arg_end(2);
    const esp_console_cmd_t forget_cmd = {
        .command = "wifi_forget",
        .help = "Drop one stored WiFi credential, and the link if it is the one in use",
        .hint = NULL,
        .func = cmd_wifi_forget,
        .argtable = &s_wifi_forget_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&forget_cmd));

    const esp_console_cmd_t known_cmd = {
        .command = "wifi_known",
        .help = "List the saved WiFi networks and which one is rejoined at boot",
        .hint = NULL,
        .func = cmd_wifi_known,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&known_cmd));
}
