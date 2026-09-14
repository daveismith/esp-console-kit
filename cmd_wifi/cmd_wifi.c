/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Console example — WiFi commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "cmd_wifi.h"

#define JOIN_TIMEOUT_MS (10000)

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;


/*
 * No esp_wifi_connect() on WIFI_EVENT_STA_DISCONNECTED here. Whatever drives joins (see
 * wifi_known.c) has to be able to tell a bad passphrase from a link that dropped -- which is
 * impossible if something else reconnects before the failure can be reported. Reconnection
 * policy lives in wifi_known.c, in one place, and this handler is left with the one job the
 * `join` command needs: telling it when an address arrived.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

/*
 * Exported so the application (or wifi_known_start()) can bring the radio up at boot rather
 * than waiting for someone to type `join`. Idempotent, and the `join` command still calls
 * it, so whichever happens first wins and the second is a no-op.
 */
void wifi_bringup(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    static bool initialized = false;
    if (initialized) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    /*
     * The application has usually created the default event loop already -- anything else
     * that posts events needs it before WiFi comes up. The upstream console_advanced example
     * created it here under ESP_ERROR_CHECK, which aborts with ESP_ERR_INVALID_STATE in that
     * case: the board rebooted the instant you tried to join a network.
     *
     * Already-created is success here: the loop is a process-wide singleton and we only
     * need it to exist.
     */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    /*
     * Power save OFF, overriding the driver's WIFI_PS_MIN_MODEM default. (There is no
     * Kconfig for this in IDF 6.1; the symbol older guides name was removed, so it has to
     * be a call.) Must follow esp_wifi_start().
     *
     * Under MIN_MODEM the radio sleeps between DTIM beacons and the AP buffers downlink
     * frames until the station wakes, which costs a flat ~124 ms on everything arriving AT
     * the board. That went unnoticed for a long time for one reason worth remembering:
     * every ping used to test it was board-initiated, and that is the one direction power
     * save cannot affect, because the station wakes itself to transmit. Measured from the
     * host TO the board, three interleaved pairs of 400 packets:
     *
     *     WIFI_PS_NONE       median  10.4 ms   mean  21.8 ms   p90  37 ms
     *     WIFI_PS_MIN_MODEM  median 123.8 ms   mean 163.5 ms   p90 332 ms
     *
     * Per-round medians 10.9/10.8/9.6 against 123.7/124.0/124.0 -- that flatness is the DTIM
     * interval, not the radio environment. (Measured on r2_domeplayer.) Inbound is the
     * direction commands and streamed data arrive on, so this is the half of the latency
     * that gates the board responding to anything at all.
     *
     * COST: the receiver stays on, which is real current draw and is NOT measured. Put a
     * meter on it before running from a battery. `wifi_ps min` reverts it live.
     */
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    initialized = true;
}

static bool wifi_join(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_bringup();
    wifi_config_t wifi_config = { 0 };
    strlcpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) {
        strlcpy((char *) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    }

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    esp_wifi_connect();

    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                                   pdFALSE, pdTRUE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & CONNECTED_BIT) != 0;
}

/** Arguments used by 'join' function */
static struct {
    struct arg_int *timeout;
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} join_args;

static int connect(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &join_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, join_args.end, argv[0]);
        return 1;
    }
    ESP_LOGI(__func__, "Connecting to '%s'",
             join_args.ssid->sval[0]);

    /* set default value*/
    if (join_args.timeout->count == 0) {
        join_args.timeout->ival[0] = JOIN_TIMEOUT_MS;
    }

    bool connected = wifi_join(join_args.ssid->sval[0],
                               join_args.password->sval[0],
                               join_args.timeout->ival[0]);
    if (!connected) {
        ESP_LOGW(__func__, "Connection timed out");
        return 1;
    }
    ESP_LOGI(__func__, "Connected");
    return 0;
}

/*
 * Transmit power, in dBm, as a live knob rather than a rebuild.
 *
 * esp_wifi_set_max_tx_power() takes quarter-dBm and the radio quantises to the driver's own
 * table, so the readback after a set is the value that matters, not the one asked for. The
 * reason to turn it DOWN is that the PA at full output is the largest current draw on the
 * board and its own worst neighbour: supply sag and die heat both push the transmitter's
 * error vector up, and a distorted frame at 20 dBm can be retried more often than a clean
 * one at 17. Whether that is true of any particular board and room is a measurement, which
 * is what this command is for -- see `ping -c 1000 -i 0.2 <gateway>`.
 *
 * Not persisted: esp_wifi_start() resets it to the Kconfig default on every boot, and a
 * radio setting that survives a reboot without appearing in the config is a trap.
 */
static struct {
    struct arg_dbl *dbm;
    struct arg_end *end;
} txpower_args;

static int wifi_txpower(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &txpower_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, txpower_args.end, argv[0]);
        return 1;
    }

    if (txpower_args.dbm->count > 0) {
        int8_t quarters = (int8_t) lrint(txpower_args.dbm->dval[0] * 4.0);
        esp_err_t err = esp_wifi_set_max_tx_power(quarters);
        if (err != ESP_OK) {
            printf("wifi_txpower: %s\n", esp_err_to_name(err));
            return 1;
        }
    }

    int8_t now = 0;
    esp_err_t err = esp_wifi_get_max_tx_power(&now);
    if (err != ESP_OK) {
        printf("wifi_txpower: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("max tx power: %.2f dBm (%d quarter-dBm)\n", now / 4.0, now);
    return 0;
}

/*
 * What the radio is actually attached to, printed in one line.
 *
 * Every measurement of this link -- a ping run, an iperf, a session that held or did not --
 * is only interpretable against the RF conditions it ran under, and those move on their
 * own: a body between the board and the AP is worth 10 dB. Without this, a firmware change
 * that coincided with a quiet channel looks like a fix.
 */
static int wifi_link(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        printf("not associated (%s)\n", esp_err_to_name(err));
        return 1;
    }

    const char *phy = ap.phy_11n ? "11n" : ap.phy_11g ? "11g" : ap.phy_11b ? "11b" : "?";
    int8_t power = 0;
    esp_wifi_get_max_tx_power(&power);
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);

    printf("ssid %-32s bssid " MACSTR "\n", (const char *) ap.ssid, MAC2STR(ap.bssid));
    printf("channel %u  rssi %d dBm  %s%s  tx power %.2f dBm  ps %s\n",
           (unsigned) ap.primary, (int) ap.rssi, phy, ap.wps ? " wps" : "",
           power / 4.0,
           ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min_modem" : "max_modem");
    return 0;
}

/*
 * Power save, as a live knob for the same reason as the transmit power above.
 *
 * The default is WIFI_PS_MIN_MODEM: the radio sleeps between DTIM beacons and the AP
 * buffers downlink frames until the station wakes. That costs latency in units of the
 * beacon interval whenever a wake-up is missed -- and a board with a busy ISR on the WiFi
 * driver's core has a specific reason to miss one. Measured on r2_domeplayer, whose RGB
 * panel runs a bounce-buffer ISR every ~500 us: ping to the gateway had a ~100-200 ms median
 * and the same packet loss as with the panel off, which is delay without loss -- the shape
 * of a missed wake window rather than of a bad link.
 *
 * Not persisted, for the same reason: esp_wifi_start() applies the Kconfig default on
 * every boot, and a radio setting that outlives a reboot without appearing in the config
 * is a trap.
 */
static struct {
    struct arg_str *mode;
    struct arg_end *end;
} ps_args;

static int wifi_ps(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &ps_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ps_args.end, argv[0]);
        return 1;
    }

    if (ps_args.mode->count > 0) {
        const char *m = ps_args.mode->sval[0];
        wifi_ps_type_t want;
        if (strcmp(m, "none") == 0) {
            want = WIFI_PS_NONE;
        } else if (strcmp(m, "min") == 0) {
            want = WIFI_PS_MIN_MODEM;
        } else if (strcmp(m, "max") == 0) {
            want = WIFI_PS_MAX_MODEM;
        } else {
            printf("wifi_ps: expected none, min or max\n");
            return 1;
        }
        esp_err_t err = esp_wifi_set_ps(want);
        if (err != ESP_OK) {
            printf("wifi_ps: %s\n", esp_err_to_name(err));
            return 1;
        }
    }

    wifi_ps_type_t now = WIFI_PS_NONE;
    esp_err_t err = esp_wifi_get_ps(&now);
    if (err != ESP_OK) {
        printf("wifi_ps: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("power save: %s\n", now == WIFI_PS_NONE ? "none"
                             : now == WIFI_PS_MIN_MODEM ? "min_modem" : "max_modem");
    return 0;
}

void register_wifi(void)
{
    join_args.timeout = arg_int0(NULL, "timeout", "<t>", "Connection timeout, ms");
    join_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of AP");
    join_args.password = arg_str0(NULL, NULL, "<pass>", "PSK of AP");
    join_args.end = arg_end(2);

    const esp_console_cmd_t join_cmd = {
        .command = "join",
        .help = "Join WiFi AP as a station",
        .hint = NULL,
        .func = &connect,
        .argtable = &join_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&join_cmd) );

    txpower_args.dbm = arg_dbl0(NULL, NULL, "<dbm>", "Maximum transmit power in dBm; omit to read it back");
    txpower_args.end = arg_end(1);

    const esp_console_cmd_t txpower_cmd = {
        .command = "wifi_txpower",
        .help = "Show or set the radio's maximum transmit power",
        .hint = NULL,
        .func = &wifi_txpower,
        .argtable = &txpower_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&txpower_cmd) );

    const esp_console_cmd_t link_cmd = {
        .command = "wifi_link",
        .help = "Show the current association: BSSID, channel, RSSI, PHY, tx power, power save",
        .hint = NULL,
        .func = &wifi_link,
        .argtable = NULL
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&link_cmd) );

    ps_args.mode = arg_str0(NULL, NULL, "<none|min|max>", "Power save mode; omit to read it back");
    ps_args.end = arg_end(1);

    const esp_console_cmd_t ps_cmd = {
        .command = "wifi_ps",
        .help = "Show or set the radio's power save mode",
        .hint = NULL,
        .func = &wifi_ps,
        .argtable = &ps_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&ps_cmd) );
}
