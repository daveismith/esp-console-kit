/*
 * wifi_ap: the board's own access point. See wifi_ap.h.
 *
 * The station keeps working while the access point runs (APSTA), and the access point then
 * follows the station's channel: the radio can only be on one. Stopping it returns the radio
 * to station mode, which wifi_known relies on.
 *
 * The passphrase is random, not derived from the MAC: the MAC is the access point's BSSID,
 * which every phone in range can see.
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "cmd_wifi.h"
#include "wifi_ap.h"

static const char *TAG = "wifi_ap";

#define NVS_NS "wifi_ap"
#define AP_IFKEY "WIFI_AP_DEF"

static bool s_on;
#if CONFIG_CMD_WIFI_AP_CAPTIVE_DNS
static char s_portal_uri[32];       /* DHCP option 114 keeps a pointer to it */
#endif
static esp_timer_handle_t s_off_timer;  /* on for a while: turns it off */
static int64_t s_off_at_us;         /* when, by esp_timer_get_time(); 0 stays on */

void wifi_ap_default_name(const char *prefix, char *out, size_t out_len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "%s-%02x%02x", prefix, mac[4], mac[5]);
}

/* Three groups of four, from letters and digits that can't be mistaken for one another on a
 * phone keyboard: 14 characters, about 72 bits. */
static void make_passphrase(char *out, size_t out_len)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    size_t n = 0;
    for (int i = 0; i < 12 && n + 2 < out_len; i++) {
        if (i > 0 && i % 4 == 0) {
            out[n++] = '-';
        }
        out[n++] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    out[n] = '\0';
}

/* The stored SSID and passphrase, or the defaults; a passphrase made now is stored. */
static void load_credentials(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    size_t len;
    ssid[0] = pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        len = 33;
        if (nvs_get_str(h, "ssid", ssid, &len) != ESP_OK) {
            ssid[0] = '\0';
        }
        len = 65;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            make_passphrase(pass, 65);
            nvs_set_str(h, "pass", pass);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    if (ssid[0] == '\0') {
        wifi_ap_default_name(CONFIG_CMD_WIFI_AP_SSID_PREFIX, ssid, 33);
    }
    if (pass[0] == '\0') {
        make_passphrase(pass, 65);      /* no NVS: this boot only */
    }
}

/* ------------------------------------------------------------------ captive DNS */

#if CONFIG_CMD_WIFI_AP_CAPTIVE_DNS
/*
 * Every A query gets the board's address; anything else gets an empty answer. Enough for a
 * phone's connectivity check to land on the board's page. It listens only on the access
 * point's address, so the station side of the board is never a DNS server. Started with the
 * first access point and left running: while the access point is off nothing reaches it.
 */
static void dns_task(void *arg)
{
    const uint32_t ip = (uint32_t)(uintptr_t)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = ip };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "captive DNS: cannot listen on port 53");
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (n < 12 || (buf[2] & 0x80) || ((buf[4] << 8) | buf[5]) < 1) {
            continue;       /* short, a response, or no question */
        }
        /* The first question: its name, then type and class */
        int p = 12;
        while (p < n && buf[p] != 0) {
            if ((buf[p] & 0xc0) != 0) {
                p = n;      /* compression in a question: not worth handling */
                break;
            }
            p += buf[p] + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        const uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int len = p + 5;                 /* the header and the one question we answer */
        buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, RD copied */
        buf[3] = 0x80;                   /* RA, no error */
        buf[4] = 0; buf[5] = 1;          /* one question */
        buf[6] = 0; buf[7] = qtype == 1 ? 1 : 0;
        memset(buf + 8, 0, 4);           /* no authority or additional records */
        if (qtype == 1) {
            static const uint8_t answer[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
            memcpy(buf + len, answer, sizeof(answer));
            memcpy(buf + len + sizeof(answer), &ip, 4);
            len += sizeof(answer) + 4;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, from_len);
    }
}
#endif

/*
 * With the captive portal, the DHCP server offers the board as DNS server and its page as the
 * portal. Without it the DHCP server is left as IDF sets it up: a board with no web page must
 * not tell a phone that joins to open one.
 */
static void setup_dhcp(esp_netif_t *ap)
{
#if CONFIG_CMD_WIFI_AP_CAPTIVE_DNS
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(ap, &ip);
    esp_netif_dhcps_stop(ap);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer = 0x02;   /* OFFER_DNS */
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    snprintf(s_portal_uri, sizeof(s_portal_uri), "http://" IPSTR "/", IP2STR(&ip.ip));
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri,
                           strlen(s_portal_uri));
    esp_netif_dhcps_start(ap);

    static bool dns_started;
    if (!dns_started) {
        dns_started = xTaskCreate(dns_task, "captive_dns", 3072, (void *)(uintptr_t)ip.ip.addr, 3, NULL) == pdPASS;
    }
#else
    (void)ap;
#endif
}

/* Up with the stored credentials, or restarted with them; the count, if any, runs on. */
static esp_err_t ap_up(void)
{
    wifi_bringup();
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(AP_IFKEY);
    if (ap == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char ssid[33], pass[65];
    load_credentials(ssid, pass);

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const wifi_mode_t want = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(want);
    if (err != ESP_OK) {
        return err;
    }
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    strlcpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = CONFIG_CMD_WIFI_AP_MAX_CONN;
    cfg.ap.channel = 1;     /* with the station joined, the station's channel wins */
    cfg.ap.pmf_cfg.capable = true;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        esp_wifi_set_mode(mode);
        return err;
    }
    setup_dhcp(ap);
    s_on = true;
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(ap, &ip);
    ESP_LOGI(TAG, "access point %s up at " IPSTR, ssid, IP2STR(&ip.ip));
    return ESP_OK;
}

static void cancel_off(void)
{
    s_off_at_us = 0;
    if (s_off_timer != NULL) {
        esp_timer_stop(s_off_timer);
    }
}

esp_err_t wifi_ap_start(void)
{
    cancel_off();
    return wifi_ap_is_on() ? ESP_OK : ap_up();
}

static void off_timer_cb(void *arg)
{
    (void)arg;
    if (s_off_at_us == 0) {
        return;     /* turned on to stay since */
    }
    ESP_LOGI(TAG, "access point off: its time is up");
    wifi_ap_stop();
}

esp_err_t wifi_ap_start_for(uint32_t seconds)
{
    if (seconds == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_ap_is_on() && s_off_at_us == 0) {
        return ESP_OK;      /* on to stay */
    }
    if (s_off_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = off_timer_cb, .name = "wifi_ap_off" };
        esp_err_t err = esp_timer_create(&args, &s_off_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = wifi_ap_is_on() ? ESP_OK : ap_up();
    if (err != ESP_OK) {
        return err;
    }
    esp_timer_stop(s_off_timer);
    s_off_at_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    err = esp_timer_start_once(s_off_timer, (uint64_t)seconds * 1000000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "access point on for %u s", (unsigned)seconds);
    }
    return err;
}

esp_err_t wifi_ap_stop(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    cancel_off();
    s_on = false;
    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
        /* Station mode even from AP alone: wifi_known joins from there */
        return esp_wifi_set_mode(WIFI_MODE_STA);
    }
    return ESP_OK;
}

bool wifi_ap_is_on(void)
{
    if (!s_on) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

void wifi_ap_get_info(wifi_ap_info_t *out)
{
    memset(out, 0, sizeof(*out));
    load_credentials(out->ssid, out->passphrase);
    out->on = wifi_ap_is_on();
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(AP_IFKEY);
    if (ap != NULL) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_get_ip_info(ap, &ip);
        out->ip = ip.ip.addr;
    }
    if (out->on) {
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
            out->channel = cfg.ap.channel;
        }
        uint8_t primary;
        wifi_second_chan_t second;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
            out->channel = primary;
        }
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            out->clients = list.num;
        }
        const int64_t off_at = s_off_at_us;
        if (off_at != 0) {
            const int64_t left = off_at - esp_timer_get_time();
            out->off_in_s = left > 0 ? (uint32_t)((left + 999999) / 1000000) : 1;
        }
    }
}

esp_err_t wifi_ap_set_credentials(const char *ssid, const char *passphrase)
{
    if (ssid != NULL && (ssid[0] == '\0' || strlen(ssid) > 32)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (passphrase != NULL && (strlen(passphrase) < 8 || strlen(passphrase) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid != NULL) {
        err = nvs_set_str(h, "ssid", ssid);
    }
    if (err == ESP_OK && passphrase != NULL) {
        err = nvs_set_str(h, "pass", passphrase);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK && wifi_ap_is_on()) {
        err = ap_up();
    }
    return err;
}

/* ------------------------------------------------------------------ console */

static void print_ap(void)
{
    wifi_ap_info_t ap;
    wifi_ap_get_info(&ap);
    if (ap.on) {
        esp_ip4_addr_t ip = { .addr = ap.ip };
        printf("ap: on, %s, channel %u, %d client%s\n", ap.ssid, ap.channel, ap.clients,
               ap.clients == 1 ? "" : "s");
        if (ap.off_in_s != 0) {
            printf("off in %u:%02u (`wifi ap on` keeps it on)\n", (unsigned)(ap.off_in_s / 60),
                   (unsigned)(ap.off_in_s % 60));
        }
        printf("pass: %s\n", ap.passphrase);
#if CONFIG_CMD_WIFI_AP_CAPTIVE_DNS
        printf("page: http://" IPSTR "/\n", IP2STR(&ip));
#else
        printf("address: " IPSTR "\n", IP2STR(&ip));
#endif
    } else {
        printf("ap: off (`wifi ap on` starts it; off again at every boot)\n");
        printf("ssid: %s\npass: %s\n", ap.ssid, ap.passphrase);
    }
}

int wifi_ap_command(int argc, char **argv)
{
    const char *ssid = NULL, *pass = NULL;
    int power = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "on") == 0) {
            power = 1;
        } else if (strcmp(argv[i], "off") == 0) {
            power = 0;
        } else if (strcmp(argv[i], "--ssid") == 0 && i + 1 < argc) {
            ssid = argv[++i];
        } else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) {
            pass = argv[++i];
        } else {
            printf("usage: wifi ap [on|off] [--ssid <ssid>] [--pass <passphrase>]\n");
            return 1;
        }
    }
    if (ssid != NULL || pass != NULL) {
        const esp_err_t err = wifi_ap_set_credentials(ssid, pass);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("wifi ap: the SSID is 1-32 characters, the passphrase 8-63\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("wifi ap: cannot store them: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    if (power == 1) {
        const esp_err_t err = wifi_ap_start();
        if (err != ESP_OK) {
            printf("wifi ap: cannot start it: %s\n", esp_err_to_name(err));
            return 1;
        }
    } else if (power == 0) {
        wifi_ap_stop();
    }
    print_ap();
    return 0;
}
