/*
 * web_net: the /api/v1/network routes. See web_net.h.
 *
 * Passphrases go in and never come out. Whatever can cut the caller off -- turning the station
 * off, joining another network, forgetting the one in use, changing the access point -- is
 * answered first and done after, so the reply gets out on the link it came in on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/inet.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_ap.h"
#include "events.h"
#include "wifi_known.h"
#include "web_net.h"
#include "web_server.h"

#define SCAN_MAX 32

static bool add_known(const char *ssid, bool has_passphrase, bool last, void *ctx)
{
    (void)has_passphrase;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddBoolToObject(o, "last", last);
    cJSON_AddItemToArray((cJSON *)ctx, o);
    return true;
}

static cJSON *network_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", web_server_hostname());
    cJSON_AddItemToObject(root, "sta", web_sta_json());
    wifi_known_list(add_known, cJSON_AddArrayToObject(root, "known"));
    wifi_ap_info_t ap;
    wifi_ap_get_info(&ap);
    memset(ap.passphrase, 0, sizeof(ap.passphrase));
    cJSON *apo = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(apo, "on", ap.on);
    cJSON_AddStringToObject(apo, "ssid", ap.ssid);
    if (ap.on) {
        char ip[16];
        esp_ip4_addr_t a = { .addr = ap.ip };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        cJSON_AddStringToObject(apo, "ip", ip);
        cJSON_AddNumberToObject(apo, "channel", ap.channel);
        cJSON_AddNumberToObject(apo, "clients", ap.clients);
        if (ap.off_in_s != 0) {
            cJSON_AddNumberToObject(apo, "off_in_s", ap.off_in_s);
        }
    }
    return root;
}

/* A string in 1..max bytes (min for a passphrase), if present: false when present and not. */
static bool str_ok(const cJSON *v, size_t min, size_t max)
{
    return v == NULL || (cJSON_IsString(v) && strlen(v->valuestring) >= min && strlen(v->valuestring) <= max);
}

static bool only(const cJSON *o, const char *a, const char *b, const char *c)
{
    for (const cJSON *k = o->child; k != NULL; k = k->next) {
        if (strcmp(k->string, a) && (b == NULL || strcmp(k->string, b)) && (c == NULL || strcmp(k->string, c))) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ /network */

static esp_err_t network_get(httpd_req_t *req)
{
    return web_send_json(req, 200, network_json());
}

static esp_err_t network_patch(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 128);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *en = cJSON_GetObjectItem(body, "sta_enabled");
    const bool ok = body->child != NULL && only(body, "sta_enabled", NULL, NULL) && cJSON_IsBool(en);
    const bool on = cJSON_IsTrue(en);
    cJSON_Delete(body);
    if (!ok) {
        return web_send_error(req, 400, "bad_request", "send {\"sta_enabled\": true or false}");
    }
    cJSON *root = network_json();
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(root, "sta"), "enabled", cJSON_CreateBool(on));
    const esp_err_t sent = web_send_json(req, 200, root);
    if (on != wifi_known_is_enabled()) {
        wifi_known_set_enabled(on);
    }
    return sent;
}

/* ------------------------------------------------------------------ /network/scan */

static void scan_job(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    wifi_known_scan_t *nets = calloc(SCAN_MAX, sizeof(*nets));
    size_t n = 0;
    const esp_err_t err = nets != NULL ? wifi_known_scan(nets, SCAN_MAX, &n) : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_WIFI_STATE) {
        web_send_error(req, 409, "busy", "the board is joining a network; try again in a moment");
    } else if (err != ESP_OK) {
        web_send_error(req, 500, "failed", "the scan failed: %s", esp_err_to_name(err));
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(root, "networks");
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
            cJSON_AddNumberToObject(o, "channel", nets[i].channel);
            cJSON_AddStringToObject(o, "auth", nets[i].auth);
            cJSON_AddBoolToObject(o, "known", nets[i].known);
            cJSON_AddItemToArray(arr, o);
        }
        web_send_json(req, 200, root);
    }
    free(nets);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    web_job_start(req, "a Wi-Fi scan", scan_job, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ /network/known */

static bool query_ssid(httpd_req_t *req, char *ssid, size_t len)
{
    if (!web_query(req, "ssid", ssid, len) || ssid[0] == '\0' || strlen(ssid) > 32) {
        web_send_error(req, 400, "bad_request", "give `ssid`, 1-32 bytes");
        return false;
    }
    return true;
}

static esp_err_t known_put(httpd_req_t *req)
{
    char ssid[40];
    if (!query_ssid(req, ssid, sizeof(ssid))) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *pass = cJSON_GetObjectItem(body, "passphrase");
    const bool ok = only(body, "passphrase", NULL, NULL) && str_ok(pass, 8, 63);
    const esp_err_t err = ok ? wifi_known_save(ssid, pass != NULL ? pass->valuestring : NULL) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_error(req, 400, "bad_request", "a passphrase is 8..63 characters; leave it out for an open network");
    }
    if (err != ESP_OK) {
        return web_send_error(req, 500, "failed", "not saved: %s", esp_err_to_name(err));
    }
    events_changed("network");
    return web_send_json(req, 200, network_json());
}

static esp_err_t known_delete(httpd_req_t *req)
{
    char ssid[40];
    if (!query_ssid(req, ssid, sizeof(ssid))) {
        return ESP_OK;
    }
    if (!wifi_known_has(ssid)) {
        return web_send_error(req, 404, "not_found", "'%s' is not a known network", ssid);
    }
    httpd_resp_set_status(req, "204 No Content");
    const esp_err_t sent = httpd_resp_send(req, NULL, 0);
    bool had = false;
    wifi_known_forget(ssid, &had);      /* after: it drops the link if it is the one in use */
    events_changed("network");
    return sent;
}

/* ------------------------------------------------------------------ /network/join */

static esp_err_t join_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(body, "passphrase");
    if (!only(body, "ssid", "passphrase", NULL) || ssid == NULL || !str_ok(ssid, 1, 32) ||
        !(pass == NULL || (cJSON_IsString(pass) && (pass->valuestring[0] == '\0' || str_ok(pass, 8, 63))))) {
        cJSON_Delete(body);
        return web_send_error(req, 400, "bad_request", "send {\"ssid\": 1-32 bytes, \"passphrase\": 8-63, or left "
                              "out for a saved or open network}");
    }
    char s[33], p[64] = "";
    strlcpy(s, ssid->valuestring, sizeof(s));
    if (pass != NULL) {
        strlcpy(p, pass->valuestring, sizeof(p));
    }
    cJSON_Delete(body);
    const esp_err_t sent = web_send_json(req, 202, network_json());
    wifi_known_join(s, p[0] ? p : NULL);
    memset(p, 0, sizeof(p));
    return sent;
}

/* ------------------------------------------------------------------ /network/ap */

static esp_err_t ap_patch(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *on = cJSON_GetObjectItem(body, "on");
    const cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(body, "passphrase");
    if (body->child == NULL || !only(body, "on", "ssid", "passphrase") || (on && !cJSON_IsBool(on)) ||
        !str_ok(ssid, 1, 32) || !str_ok(pass, 8, 63)) {
        cJSON_Delete(body);
        return web_send_error(req, 400, "bad_request", "send `on` (true or false), `ssid` (1-32 bytes) or "
                              "`passphrase` (8-63 characters)");
    }
    char s[33] = "", p[64] = "";
    if (ssid != NULL) {
        strlcpy(s, ssid->valuestring, sizeof(s));
    }
    if (pass != NULL) {
        strlcpy(p, pass->valuestring, sizeof(p));
    }
    const int want = on == NULL ? -1 : cJSON_IsTrue(on);
    cJSON_Delete(body);

    /* The reply says what it will be; then it is done, which may drop an access-point client. */
    cJSON *root = network_json();
    cJSON *apo = cJSON_GetObjectItem(root, "ap");
    if (want >= 0) {
        cJSON_ReplaceItemInObject(apo, "on", cJSON_CreateBool(want));
        cJSON_DeleteItemFromObject(apo, "off_in_s");    /* on now stays on */
    }
    if (want == 0) {
        cJSON_DeleteItemFromObject(apo, "ip");      /* as GET says of an access point that is off */
        cJSON_DeleteItemFromObject(apo, "channel");
        cJSON_DeleteItemFromObject(apo, "clients");
    }
    if (s[0]) {
        cJSON_ReplaceItemInObject(apo, "ssid", cJSON_CreateString(s));
    }
    const esp_err_t sent = web_send_json(req, 200, root);
    if (s[0] || p[0]) {
        wifi_ap_set_credentials(s[0] ? s : NULL, p[0] ? p : NULL);
        memset(p, 0, sizeof(p));
    }
    if (want == 1) {
        wifi_ap_start();
    } else if (want == 0 && wifi_ap_is_on()) {
        wifi_ap_stop();
    }
    return sent;
}

/* The station and the access point coming and going; not a scan finishing. */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_SCAN_DONE || id == WIFI_EVENT_STA_BEACON_TIMEOUT)) {
        return;
    }
    events_changed("network");
}

esp_err_t web_net_register(void)
{
    web_server_add_feature("network");
    events_declare("network", network_json);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_err_t err = ESP_OK;
    err |= web_register("/api/v1/network", HTTP_GET, network_get, 0);
    err |= web_register("/api/v1/network", HTTP_PATCH, network_patch, WEB_AUTH);
    err |= web_register("/api/v1/network/scan", HTTP_GET, scan_get, 0);
    err |= web_register("/api/v1/network/known", HTTP_PUT, known_put, WEB_AUTH);
    err |= web_register("/api/v1/network/known", HTTP_DELETE, known_delete, WEB_AUTH);
    err |= web_register("/api/v1/network/join", HTTP_POST, join_post, WEB_AUTH);
    err |= web_register("/api/v1/network/ap", HTTP_PATCH, ap_patch, WEB_AUTH);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}
