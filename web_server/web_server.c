/*
 * web_server: the board's HTTP server. See web_server.h.
 *
 * Protection, for a device on a home network with an optional password:
 *
 *  - Every request that changes something (PUT, POST, PATCH, DELETE) must name the board in its Host
 *    header -- an IP address, `<hostname>` or `<hostname>.local`. A web page elsewhere that
 *    rebinds its own DNS name to the board's address still sends its own name, so it is
 *    refused.
 *  - A POST or PATCH must say it carries JSON (Content-Type application/json). A page elsewhere
 *    can only send that -- or a PUT, PATCH or DELETE at all -- after a CORS preflight, which this server
 *    approves only for the allowlist below, so a page the user happens to visit cannot drive
 *    the board -- even with no password set.
 *  - With a password set (`web password`), routes registered WEB_AUTH also need it, as HTTP
 *    Basic (any user name) or as a Bearer token. Reading is open, but for the few GETs that
 *    read something private (a file, the web settings), which are WEB_AUTH too.
 *  - CORS: pages from the origins on an allowlist -- CONFIG_WEB_SERVER_CORS_ORIGINS, or what
 *    `web cors` stored -- may call the API from a browser (API tools such as Swagger Editor, a
 *    project's own documentation). Their preflights are answered and their replies carry
 *    Access-Control-Allow-Origin; every other origin is refused as before. The checks above
 *    still apply to them, so with no password an allowed site can drive the board: `web cors`
 *    says so.
 *
 * Unknown paths: an embedded asset if there is one; a JSON 404 under /api; otherwise, for a
 * request that came in on the access point, a redirect to the board's page -- which is what
 * makes a phone's connectivity check open it.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "lwip/inet.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ota_core.h"
#include "wifi_ap.h"
#include "wifi_known.h"
#include "web_internal.h"
#include "web_server.h"

static const char *TAG = "web";

#define NVS_NS "web"
#define MAX_FEATURES 16

typedef struct {
    char uri[48];
    httpd_method_t method;
    web_handler_t handler;
    unsigned flags;
    bool live;                  /* registered with the running server */
    bool preflight;             /* this route also registered the path's OPTIONS handler */
} route_t;

static web_server_config_t s_cfg;
static httpd_handle_t s_server;
static route_t s_routes[CONFIG_WEB_SERVER_MAX_ROUTES];
static size_t s_n_routes;
static const char *s_features[MAX_FEATURES];
static size_t s_n_features;
static char s_hostname[33];
static char s_etag[20];
static bool s_mdns_up;
static char s_cors[CONFIG_WEB_SERVER_CORS_MAX_LEN];   /* space-separated origins */

/* ------------------------------------------------------------------ settings */

static bool nvs_str(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    const bool ok = nvs_get_str(h, key, out, &n) == ESP_OK;
    nvs_close(h);
    if (!ok) {
        out[0] = '\0';
    }
    return ok && out[0] != '\0';
}

static esp_err_t nvs_put_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = value != NULL ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool stored_off(void)
{
    char v[4];
    return nvs_str("off", v, sizeof(v)) && v[0] == '1';
}

static bool have_password(void)
{
    char pw[65];
    return nvs_str("password", pw, sizeof(pw));
}

/* The CORS allowlist: the stored one if `web cors` ever changed it (even to nothing), else the
 * build's default. */
static void load_cors(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_cors);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        const esp_err_t err = nvs_get_str(h, "cors", s_cors, &n);
        nvs_close(h);
        if (err == ESP_OK) {
            return;
        }
    }
    strlcpy(s_cors, CONFIG_WEB_SERVER_CORS_ORIGINS, sizeof(s_cors));
}

static bool cors_stored(void)
{
    nvs_handle_t h;
    size_t n = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool stored = nvs_get_str(h, "cors", NULL, &n) == ESP_OK;
    nvs_close(h);
    return stored;
}

/*
 * `origin` (scheme://host[:port], from the Origin header) against one allowlist entry: the same,
 * case aside; a host of `*.domain` (after the scheme), any subdomain of domain at any depth, not
 * domain itself; or `*`, anything.
 */
static bool origin_matches(const char *origin, const char *pat, size_t pat_len)
{
    if (pat_len == 1 && pat[0] == '*') {
        return true;
    }
    const char *os = strstr(origin, "://");
    const char *ps = NULL;
    for (size_t i = 0; i + 3 <= pat_len; i++) {
        if (strncmp(pat + i, "://", 3) == 0) {
            ps = pat + i;
            break;
        }
    }
    if (os == NULL || ps == NULL || os - origin != ps - pat || strncasecmp(origin, pat, os - origin) != 0) {
        return false;
    }
    const char *oh = os + 3;
    const char *ph = ps + 3;
    const size_t phl = pat_len - (ph - pat);
    if (oh[0] == '\0' || strpbrk(oh, "/@\\ ") != NULL) {
        return false;       /* an Origin is scheme://host[:port], nothing more */
    }
    if (phl > 2 && ph[0] == '*' && ph[1] == '.') {
        const size_t sl = phl - 1;              /* ".domain" */
        const size_t ol = strlen(oh);
        return ol > sl && oh[ol - sl - 1] != '.' && strncasecmp(oh + ol - sl, ph + 1, sl) == 0;
    }
    return strlen(oh) == phl && strncasecmp(oh, ph, phl) == 0;
}

static bool origin_allowed(const char *origin)
{
    for (const char *p = s_cors; *p; ) {
        while (*p == ' ') {
            p++;
        }
        const size_t n = strcspn(p, " ");
        if (n > 0 && origin_matches(origin, p, n)) {
            return true;
        }
        p += n;
    }
    return false;
}

/* The request's Origin, when it is on the allowlist. The CORS headers point into `c`, which must
 * live until the reply is sent. */
typedef struct {
    char origin[128];
} cors_t;

bool web_cors_origin(httpd_req_t *req, char *out, size_t len)
{
    return httpd_req_get_hdr_value_str(req, "Origin", out, len) == ESP_OK && origin_allowed(out);
}

static bool cors_headers(httpd_req_t *req, cors_t *c)
{
    if (httpd_req_get_hdr_value_str(req, "Origin", c->origin, sizeof(c->origin)) != ESP_OK ||
        !origin_allowed(c->origin)) {
        return false;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", c->origin);
    httpd_resp_set_hdr(req, "Vary", "Origin");
    return true;
}

/* An allowlist entry: `*`, or scheme://host[:port] with an optional `*.` before the host. */
static bool valid_origin(const char *o)
{
    if (strcmp(o, "*") == 0) {
        return true;
    }
    const char *s = strstr(o, "://");
    if (s == NULL || s == o || strlen(o) >= 96) {
        return false;
    }
    for (const char *p = o; p < s; p++) {
        if (!isalpha((unsigned char)*p)) {
            return false;
        }
    }
    const char *h = s + 3;
    if (strncmp(h, "*.", 2) == 0) {
        h += 2;
    }
    if (*h == '\0' || *h == '.' || *h == ':') {
        return false;
    }
    for (; *h; h++) {
        if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != ':') {
            return false;
        }
    }
    return true;
}

/* A hostname: 1-32 letters, digits and inner hyphens. */
static bool valid_hostname(const char *h)
{
    const size_t n = strlen(h);
    bool ok = n >= 1 && n <= 32 && h[0] != '-' && h[n - 1] != '-';
    for (size_t i = 0; ok && i < n; i++) {
        ok = isalnum((unsigned char)h[i]) || h[i] == '-';
    }
    return ok;
}

const char *web_server_hostname(void)
{
    return s_hostname;
}

static void load_hostname(void)
{
    if (!nvs_str("hostname", s_hostname, sizeof(s_hostname))) {
        wifi_ap_default_name(s_cfg.name_prefix ? s_cfg.name_prefix : "esp", s_hostname, sizeof(s_hostname));
    }
}

static void apply_hostname(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != NULL) {
        esp_netif_set_hostname(sta, s_hostname);     /* DHCP: from the next lease */
    }
    if (s_mdns_up) {
        mdns_hostname_set(s_hostname);
    }
}

static void start_mdns(void)
{
    if (s_mdns_up) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable");
        return;
    }
    s_mdns_up = true;
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(s_cfg.product ? s_cfg.product : s_hostname);
    mdns_txt_item_t txt[] = { { "path", "/" }, { "api", "/api/v1" } };
    mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
}

/* ------------------------------------------------------------------ helpers */

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 302: return "302 Found";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 411: return "411 Length Required";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Content";
    case 503: return "503 Service Unavailable";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

esp_err_t web_send_json(httpd_req_t *req, int status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    cors_t cors;
    if (cors_headers(req, &cors)) {
        httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "WWW-Authenticate");
    }
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

esp_err_t web_send_error(httpd_req_t *req, int status, const char *code, const char *fmt, ...)
{
    char message[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", message);
    if (status == 401) {
        /* Bearer, not Basic: a browser does not pop up its own login box for it */
        char realm[64];
        snprintf(realm, sizeof(realm), "Bearer realm=\"%s\"", s_hostname);
        httpd_resp_set_hdr(req, "WWW-Authenticate", realm);
        return web_send_json(req, status, root);    /* realm lives until the send */
    }
    return web_send_json(req, status, root);
}

cJSON *web_read_json(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0) {
        return cJSON_CreateObject();
    }
    if (req->content_len > max_len) {
        web_send_error(req, 413, "too_large", "the body is over %u bytes", (unsigned)max_len);
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return NULL;    /* the connection is gone; nothing to answer */
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        web_send_error(req, 400, "bad_json", "the body is not a JSON object");
        return NULL;
    }
    return root;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A query value as sent (%2F, +) to what it says, cut short to fit. A %00 ends it. */
static void url_decode(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    while (*in && n + 1 < out_len) {
        char c = *in++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && hex_value(in[0]) >= 0 && hex_value(in[1]) >= 0) {
            c = (char)(hex_value(in[0]) << 4 | hex_value(in[1]));
            in += 2;
            if (c == '\0') {
                break;
            }
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

bool web_query(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    const size_t len = httpd_req_get_url_query_len(req);
    if (len == 0 || len > 512) {
        return false;
    }
    char query[513];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[513];
    if (httpd_query_key_value(query, key, raw, sizeof(raw)) == ESP_OK) {
        url_decode(raw, out, out_len);
        return true;
    }
    /* A bare key ("?activate") counts as present and empty */
    const size_t klen = strlen(key);
    for (const char *p = query; *p; ) {
        if (strncmp(p, key, klen) == 0 && (p[klen] == '&' || p[klen] == '\0')) {
            return true;
        }
        p = strchr(p, '&');
        if (p == NULL) {
            break;
        }
        p++;
    }
    return false;
}

bool web_query_bool(httpd_req_t *req, const char *key, bool dflt)
{
    char v[8];
    if (!web_query(req, key, v, sizeof(v))) {
        return dflt;
    }
    return v[0] == '\0' || strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0;
}

static uint32_t local_ipv4(httpd_req_t *req)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        /* An IPv4 client of the dual-stack socket: ::ffff:a.b.c.d */
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&addr;
        const uint32_t *w = (const uint32_t *)&a6->sin6_addr;
        if (w[0] == 0 && w[1] == 0 && w[2] == htonl(0xffff)) {
            return w[3];
        }
    }
#endif
    return 0;
}

bool web_req_via_ap(httpd_req_t *req)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (ap == NULL || !wifi_ap_is_on() || esp_netif_get_ip_info(ap, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0 && local_ipv4(req) == ip.ip.addr;
}

cJSON *web_app_desc_json(const esp_app_desc_t *d)
{
    cJSON *o = cJSON_CreateObject();
    char elf[65];
    for (int i = 0; i < 32; i++) {
        snprintf(elf + 2 * i, 3, "%02x", d->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(o, "project", d->project_name);
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddStringToObject(o, "date", d->date);
    cJSON_AddStringToObject(o, "time", d->time);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    cJSON_AddStringToObject(o, "elf_sha256", elf);
    return o;
}

/* ------------------------------------------------------------------ assets */

static const web_asset_t *find_asset(const char *path, size_t len)
{
    if (len == 1 && path[0] == '/') {
        path = "/index.html";
        len = strlen(path);
    }
    for (size_t i = 0; i < s_cfg.n_assets; i++) {
        const web_asset_t *a = &s_cfg.assets[i];
        if (strlen(a->path) == len && strncmp(a->path, path, len) == 0) {
            return a;
        }
    }
    return NULL;
}

static esp_err_t send_asset(httpd_req_t *req, const web_asset_t *a)
{
    /* The assets are part of the image: they change exactly when the image does */
    char tag[24];
    snprintf(tag, sizeof(tag), "\"%s\"", s_etag);
    cors_t cors;
    cors_headers(req, &cors);   /* /api/v1/openapi.json, for an API tool to load */
    char seen[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", seen, sizeof(seen)) == ESP_OK &&
        strcmp(seen, tag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", tag);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "ETag", tag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (a->gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    return httpd_resp_send(req, (const char *)a->data, a->len);
}

esp_err_t web_send_asset(httpd_req_t *req, const char *path)
{
    const web_asset_t *a = find_asset(path, strlen(path));
    if (a == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return send_asset(req, a);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    const char *q = strchr(req->uri, '?');
    const size_t len = q ? (size_t)(q - req->uri) : strlen(req->uri);
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        const web_asset_t *a = find_asset(req->uri, len);
        if (a != NULL) {
            return send_asset(req, a);
        }
    }
    if (strncmp(req->uri, "/api/", 5) == 0) {
        return web_send_error(req, 404, "not_found", "no such endpoint: %.*s", (int)len, req->uri);
    }
    if (web_req_via_ap(req)) {
        /* A phone's connectivity check (generate_204, hotspot-detect.html, ...): send it to the
         * page, with a body, which iOS needs to decide there is a portal. */
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip);
        char location[32];
        snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&ip.ip));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_sendstr(req, "<a href=\"/\">Holo Player</a>");
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "not found");
}

/* ------------------------------------------------------------------ routes */

/* The Host header names the board: an address, or its own name. */
static bool host_is_ours(httpd_req_t *req)
{
    char host[80];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    if (host[0] == '[') {
        return true;                            /* an IPv6 literal */
    }
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
    }
    bool dotted = host[0] != '\0';
    for (const char *p = host; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') {
            dotted = false;
            break;
        }
    }
    if (dotted) {
        return true;
    }
    const size_t n = strlen(s_hostname);
    return strncasecmp(host, s_hostname, n) == 0 &&
           (host[n] == '\0' || strcasecmp(host + n, ".local") == 0);
}

static bool content_type_is(httpd_req_t *req, const char *type)
{
    char ct[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return false;
    }
    return strncasecmp(ct, type, strlen(type)) == 0;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, char *out, size_t out_len)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '='; in++) {
        const int v = b64_value(*in);
        if (v < 0) {
            return 0;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= out_len) {
                return 0;
            }
            out[n++] = (char)((acc >> bits) & 0xff);
        }
    }
    out[n] = '\0';
    return n;
}

/* Compare without stopping at the first difference */
static bool same_secret(const char *a, const char *b)
{
    const size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la ^ lb;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[lb ? i % lb : 0];
    }
    return diff == 0;
}

static bool authorised(httpd_req_t *req)
{
    char pw[65];
    if (!nvs_str("password", pw, sizeof(pw))) {
        return true;
    }
    char hdr[160];
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        if (strncasecmp(hdr, "Bearer ", 7) == 0) {
            ok = same_secret(hdr + 7, pw);
        } else if (strncasecmp(hdr, "Basic ", 6) == 0) {
            char plain[120];
            if (b64_decode(hdr + 6, plain, sizeof(plain)) > 0) {
                const char *colon = strchr(plain, ':');
                ok = colon != NULL && same_secret(colon + 1, pw);
            }
        }
    }
    memset(pw, 0, sizeof(pw));
    return ok;
}

static esp_err_t dispatch(httpd_req_t *req)
{
    const route_t *r = req->user_ctx;
    const bool mutating = req->method == HTTP_POST || req->method == HTTP_PUT || req->method == HTTP_PATCH ||
                          req->method == HTTP_DELETE;
    if (mutating) {
        if (!host_is_ours(req)) {
            return web_send_error(req, 403, "forbidden_host",
                                  "the Host header must be the board's address or %s.local", s_hostname);
        }
        if ((req->method == HTTP_POST || req->method == HTTP_PATCH) && !content_type_is(req, "application/json")) {
            return web_send_error(req, 415, "content_type", "a POST or PATCH must be Content-Type: application/json");
        }
    }
    if ((r->flags & WEB_AUTH) && !authorised(req)) {
        vTaskDelay(pdMS_TO_TICKS(500));     /* a guess at a time */
        return web_send_error(req, 401, "auth_required", "this needs the board's web password");
    }
    return r->handler(req);
}

/* A CORS preflight: which methods and headers an allowed origin may use. */
static esp_err_t preflight(httpd_req_t *req)
{
    cors_t cors;
    if (!cors_headers(req, &cors)) {
        return web_send_error(req, 403, "cors", "this site is not allowed to call the board (`web cors` on its console)");
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    char pna[8];
    if (httpd_req_get_hdr_value_str(req, "Access-Control-Request-Private-Network", pna, sizeof(pna)) == ESP_OK) {
        /* Chrome's consent for a public site to reach a device on the local network */
        httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    }
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t register_live(route_t *r)
{
    const httpd_uri_t uri = { .uri = r->uri, .method = r->method, .handler = dispatch, .user_ctx = r };
    esp_err_t err = httpd_register_uri_handler(s_server, &uri);
    r->live = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", r->uri, esp_err_to_name(err));
        return err;
    }
    /* One OPTIONS handler a path, for CORS preflights, with the path's first route */
    r->preflight = true;
    for (route_t *o = s_routes; o < r; o++) {
        if (o->preflight && o->live && strcmp(o->uri, r->uri) == 0) {
            r->preflight = false;
            break;
        }
    }
    if (r->preflight) {
        const httpd_uri_t opt = { .uri = r->uri, .method = HTTP_OPTIONS, .handler = preflight };
        err = httpd_register_uri_handler(s_server, &opt);
        if (err != ESP_OK) {
            r->preflight = false;
            ESP_LOGE(TAG, "OPTIONS %s: %s", r->uri, esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t web_register(const char *uri, httpd_method_t method, web_handler_t handler, unsigned flags)
{
    if (s_n_routes >= CONFIG_WEB_SERVER_MAX_ROUTES || strlen(uri) >= sizeof(s_routes[0].uri)) {
        ESP_LOGE(TAG, "no room for route %s (CONFIG_WEB_SERVER_MAX_ROUTES)", uri);
        return ESP_ERR_NO_MEM;
    }
    route_t *r = &s_routes[s_n_routes++];
    strlcpy(r->uri, uri, sizeof(r->uri));
    r->method = method;
    r->handler = handler;
    r->flags = flags;
    return s_server != NULL ? register_live(r) : ESP_OK;
}

void web_server_add_feature(const char *name)
{
    if (s_n_features < MAX_FEATURES) {
        s_features[s_n_features++] = name;
    }
}

/* ------------------------------------------------------------------ long operations */

typedef struct {
    httpd_req_t *req;
    web_job_fn_t fn;
    void *ctx;
} job_t;

static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *s_job;       /* what is running, or NULL */

static bool update_running(void)
{
    ota_core_session_t s;
    ota_core_get(&s);
    return s.state == OTA_CORE_RECEIVING || s.state == OTA_CORE_VERIFYING;
}

const char *web_job_running(void)
{
    return s_job;
}

void web_job_release(void)
{
    taskENTER_CRITICAL(&s_job_lock);
    s_job = NULL;
    taskEXIT_CRITICAL(&s_job_lock);
}

bool web_job_claim(httpd_req_t *req, const char *what)
{
    const char *running = NULL;
    taskENTER_CRITICAL(&s_job_lock);
    if (s_job != NULL) {
        running = s_job;
    } else {
        s_job = what;
    }
    taskEXIT_CRITICAL(&s_job_lock);
    if (running == NULL && update_running()) {
        web_job_release();
        running = "a firmware update";      /* a pull, or `ota put` on the console */
    }
    if (running == NULL) {
        return true;
    }
    if (req != NULL) {
        web_send_error(req, 409, "busy", "%s is in progress; try again when it is done", running);
    }
    return false;
}

static void job_task(void *arg)
{
    job_t *job = arg;
    job->fn(job->req, job->ctx);
    httpd_req_async_handler_complete(job->req);
    free(job);
    web_job_release();
    vTaskDelete(NULL);
}

esp_err_t web_job_run(httpd_req_t *req, web_job_fn_t fn, void *ctx)
{
    job_t *job = malloc(sizeof(*job));
    httpd_req_t *copy = NULL;
    if (job != NULL && httpd_req_async_handler_begin(req, &copy) == ESP_OK) {
        *job = (job_t){ .req = copy, .fn = fn, .ctx = ctx };
        if (xTaskCreate(job_task, "web_job", CONFIG_WEB_SERVER_UPLOAD_STACK_SIZE, job, 5, NULL) == pdPASS) {
            return ESP_OK;
        }
        httpd_req_async_handler_complete(copy);
    }
    free(job);
    /* No task: run it here, holding up the server until it is done */
    fn(req, ctx);
    web_job_release();
    return ESP_OK;
}

esp_err_t web_job_start(httpd_req_t *req, const char *what, web_job_fn_t fn, void *ctx)
{
    if (!web_job_claim(req, what)) {
        return ESP_ERR_INVALID_STATE;
    }
    return web_job_run(req, fn, ctx);
}

/* ------------------------------------------------------------------ built-in routes */

static void add_ip(cJSON *o, const char *key, esp_ip4_addr_t ip)
{
    char s[16];
    snprintf(s, sizeof(s), IPSTR, IP2STR(&ip));
    cJSON_AddStringToObject(o, key, s);
}

cJSON *web_sta_json(void)
{
    cJSON *sta = cJSON_CreateObject();
    cJSON_AddBoolToObject(sta, "enabled", wifi_known_is_enabled());
    esp_netif_t *sn = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t rec;
    const bool linked = sn != NULL && esp_netif_is_netif_up(sn) && esp_wifi_sta_get_ap_info(&rec) == ESP_OK;
    cJSON_AddBoolToObject(sta, "connected", linked);
    if (linked) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_get_ip_info(sn, &ip);
        cJSON_AddStringToObject(sta, "ssid", (const char *)rec.ssid);
        add_ip(sta, "ip", ip.ip);
        cJSON_AddNumberToObject(sta, "rssi", rec.rssi);
        cJSON_AddNumberToObject(sta, "channel", rec.primary);
#if CONFIG_LWIP_IPV6
        cJSON *v6 = cJSON_AddArrayToObject(sta, "ipv6");
        esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        const int n = esp_netif_get_all_ip6(sn, addrs);
        for (int i = 0; i < n; i++) {
            char s[48];
            if (inet_ntop(AF_INET6, addrs[i].addr, s, sizeof(s)) == NULL) {
                continue;
            }
            for (char *p = s; *p; p++) {
                *p = tolower((unsigned char)*p);     /* RFC 5952; lwIP writes capitals */
            }
            cJSON_AddItemToArray(v6, cJSON_CreateString(s));
        }
#endif
    }
    return sta;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api", 1);
    cJSON_AddItemToObject(root, "firmware", web_app_desc_json(esp_app_get_description()));
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "hostname", s_hostname);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON *features = cJSON_AddArrayToObject(root, "features");
    for (size_t i = 0; i < s_n_features; i++) {
        cJSON_AddItemToArray(features, cJSON_CreateString(s_features[i]));
    }
    cJSON_AddBoolToObject(root, "auth", have_password());
    cJSON_AddStringToObject(root, "via", web_req_via_ap(req) ? "ap" : "sta");

    cJSON_AddItemToObject(root, "sta", web_sta_json());

    wifi_ap_info_t ap;
    wifi_ap_get_info(&ap);
    cJSON *apo = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(apo, "on", ap.on);
    if (ap.on) {
        cJSON_AddStringToObject(apo, "ssid", ap.ssid);
        add_ip(apo, "ip", (esp_ip4_addr_t){ .addr = ap.ip });
        cJSON_AddNumberToObject(apo, "clients", ap.clients);
        if (ap.off_in_s != 0) {
            cJSON_AddNumberToObject(apo, "off_in_s", ap.off_in_s);
        }
    }
    return web_send_json(req, 200, root);
}

static esp_err_t restart_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *d = cJSON_GetObjectItem(body, "delay_ms");
    uint32_t delay = cJSON_IsNumber(d) && d->valuedouble >= 0 ? (uint32_t)d->valuedouble : 500;
    cJSON_Delete(body);
    if (delay < 200) {
        delay = 200;        /* long enough for this reply to leave */
    }
    if (delay > 60000) {
        delay = 60000;
    }
    ota_core_restart_after(delay);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "restart_in_ms", delay);
    return web_send_json(req, 202, root);
}

static esp_err_t openapi_get(httpd_req_t *req)
{
    if (web_send_asset(req, "/openapi.json") == ESP_ERR_NOT_FOUND) {
        return web_send_error(req, 404, "not_found", "this firmware carries no API description");
    }
    return ESP_OK;
}

/* A space-separated list of origins, as a JSON array. */
static void add_origins(cJSON *arr, const char *list)
{
    for (const char *p = list; *p; ) {
        while (*p == ' ') {
            p++;
        }
        const size_t n = strcspn(p, " ");
        if (n > 0) {
            char o[96];
            snprintf(o, sizeof(o), "%.*s", (int)n, p);
            cJSON_AddItemToArray(arr, cJSON_CreateString(o));
        }
        p += n;
    }
}

static cJSON *web_settings_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hostname", s_hostname);
    cJSON_AddBoolToObject(o, "auth", have_password());
    add_origins(cJSON_AddArrayToObject(o, "cors"), s_cors);
    add_origins(cJSON_AddArrayToObject(o, "cors_default"), CONFIG_WEB_SERVER_CORS_ORIGINS);
    return o;
}

static esp_err_t web_get(httpd_req_t *req)
{
    return web_send_json(req, 200, web_settings_json());
}

/* PATCH /api/v1/web: what `web hostname`, `web password` and `web cors` do, all checked before
 * any is stored. */
static esp_err_t web_patch(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 1024);
    if (body == NULL) {
        return ESP_OK;
    }
    const cJSON *host = cJSON_GetObjectItem(body, "hostname");
    const cJSON *pw = cJSON_GetObjectItem(body, "password");
    const cJSON *cors = cJSON_GetObjectItem(body, "cors");
    char list[sizeof(s_cors)] = "";
    const char *bad = NULL;
    for (const cJSON *k = body->child; k != NULL && bad == NULL; k = k->next) {
        if (strcmp(k->string, "hostname") != 0 && strcmp(k->string, "password") != 0 && strcmp(k->string, "cors") != 0) {
            bad = "only hostname, password and cors can be changed";
        }
    }
    if (body->child == NULL) {
        bad = "send hostname, password or cors";
    }
    if (bad == NULL && host != NULL && !cJSON_IsNull(host) &&
        !(cJSON_IsString(host) && valid_hostname(host->valuestring))) {
        bad = "a hostname is 1-32 letters, digits and inner hyphens, or null for the default";
    }
    if (bad == NULL && pw != NULL && !cJSON_IsNull(pw) &&
        !(cJSON_IsString(pw) && strlen(pw->valuestring) >= 4 && strlen(pw->valuestring) <= 64)) {
        bad = "a password is 4-64 characters, or null to clear it";
    }
    if (bad == NULL && cors != NULL && !cJSON_IsNull(cors)) {
        if (!cJSON_IsArray(cors)) {
            bad = "cors is a list of origins, or null for the default";
        }
        const cJSON *e;
        cJSON_ArrayForEach(e, cors) {
            char origin[96];
            if (bad != NULL) {
                break;
            }
            if (!cJSON_IsString(e) || strlen(e->valuestring) >= sizeof(origin)) {
                bad = "an origin is scheme://host[:port], as https://editor.swagger.io";
                break;
            }
            strlcpy(origin, e->valuestring, sizeof(origin));
            const size_t ol = strlen(origin);
            if (ol > 0 && origin[ol - 1] == '/') {
                origin[ol - 1] = '\0';
            }
            if (!valid_origin(origin)) {
                bad = "an origin is scheme://host[:port], as https://editor.swagger.io; https://*.example.com "
                      "for its subdomains; or *";
            } else if (strlen(list) + strlen(origin) + 2 > sizeof(list)) {
                bad = "the list is too long (CONFIG_WEB_SERVER_CORS_MAX_LEN)";
            } else {
                snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", list[0] ? " " : "", origin);
            }
        }
    }
    if (bad != NULL) {
        cJSON_Delete(body);
        return web_send_error(req, 400, "bad_request", "%s", bad);
    }

    esp_err_t err = ESP_OK;
    if (host != NULL) {
        err = nvs_put_str("hostname", cJSON_IsNull(host) ? NULL : host->valuestring);
        load_hostname();
        apply_hostname();
    }
    if (err == ESP_OK && pw != NULL) {
        err = nvs_put_str("password", cJSON_IsNull(pw) ? NULL : pw->valuestring);
    }
    if (err == ESP_OK && cors != NULL) {
        err = nvs_put_str("cors", cJSON_IsNull(cors) ? NULL : list);
        load_cors();
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return web_send_error(req, 500, "failed", "cannot store it: %s", esp_err_to_name(err));
    }
    return web_send_json(req, 200, web_settings_json());
}

/* ------------------------------------------------------------------ server */

httpd_handle_t web_server_handle(void)
{
    return s_server;
}

/* Every connection that closes: an event stream may have been on it. */
static void closed(httpd_handle_t hd, int fd)
{
    (void)hd;
    web_events_closed(fd);
    close(fd);
}

static esp_err_t start_httpd(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = CONFIG_WEB_SERVER_STACK_SIZE;
    config.max_uri_handlers = 2 * CONFIG_WEB_SERVER_MAX_ROUTES;   /* and one OPTIONS a path */
    config.max_open_sockets = CONFIG_WEB_SERVER_MAX_SOCKETS;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.close_fn = closed;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    web_events_started();
    for (size_t i = 0; i < s_n_routes; i++) {
        register_live(&s_routes[i]);
    }
    ESP_LOGI(TAG, "serving http://%s.local/ (%u routes, %u pages)", s_hostname, (unsigned)s_n_routes,
             (unsigned)s_cfg.n_assets);
    return ESP_OK;
}

static void stop_httpd(void)
{
    if (s_server != NULL) {
        web_events_stopped();
        httpd_stop(s_server);
        s_server = NULL;
        for (size_t i = 0; i < s_n_routes; i++) {
            s_routes[i].live = false;
            s_routes[i].preflight = false;
        }
    }
}

bool web_server_running(void)
{
    return s_server != NULL;
}

esp_err_t web_server_start(const web_server_config_t *cfg)
{
    s_cfg = *cfg;
    char elf[17];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    strlcpy(s_etag, elf, sizeof(s_etag));
    load_hostname();
    load_cors();

    static bool builtins;
    if (!builtins) {
        builtins = true;
        web_register("/api/v1/info", HTTP_GET, info_get, 0);
        web_register("/api/v1/restart", HTTP_POST, restart_post, WEB_AUTH);
        web_register("/api/v1/openapi.json", HTTP_GET, openapi_get, 0);
        web_register("/api/v1/web", HTTP_GET, web_get, WEB_AUTH);
        web_register("/api/v1/web", HTTP_PATCH, web_patch, WEB_AUTH);
        web_register("/api/v1/events", HTTP_GET, web_events_get, 0);
        web_events_init();
    }

    start_mdns();
    apply_hostname();
    if (stored_off()) {
        ESP_LOGI(TAG, "off (`web on` starts it)");
        return ESP_OK;
    }
    return start_httpd();
}

/* ------------------------------------------------------------------ console */

static void print_web(void)
{
    printf("web: %s\n", s_server != NULL ? "on" : stored_off() ? "off, until `web on`" : "off");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (sta != NULL && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
        ip.ip.addr != 0) {
        printf("  http://" IPSTR "/\n", IP2STR(&ip.ip));
    }
    printf("  http://%s.local/%s\n", s_hostname, s_mdns_up ? "" : "  (mDNS unavailable)");
    wifi_ap_info_t ap;
    wifi_ap_get_info(&ap);
    if (ap.on) {
        esp_ip4_addr_t a = { .addr = ap.ip };
        printf("  http://" IPSTR "/  on the access point %s\n", IP2STR(&a), ap.ssid);
    }
    printf("password: %s\n", have_password() ? "set: changes need it" : "none: anyone on the network can change the board");
    printf("cors: %s\n", s_cors[0] ? s_cors : "none: only the board's own pages call its API from a browser");
}

static void print_cors(void)
{
    printf("cors: %s%s\n", s_cors[0] ? s_cors : "none", cors_stored() ? "" : " (the default)");
    if (s_cors[0]) {
        printf("pages from these origins may call the API from a browser%s\n",
               have_password() ? "; changes still need the password"
                               : ". With no password set they can also update and restart the board: "
                                 "`web password` guards that");
    }
}

/* web cors [add <origin> | remove <origin> | reset | none] */
static int cors_cmd(int argc, char **argv)
{
    if (argc == 0) {
        print_cors();
        return 0;
    }
    char list[sizeof(s_cors)];
    if (argc == 1 && strcmp(argv[0], "reset") == 0) {
        nvs_put_str("cors", NULL);
        load_cors();
        print_cors();
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "none") == 0) {
        list[0] = '\0';
    } else if (argc == 2 && (strcmp(argv[0], "add") == 0 || strcmp(argv[0], "remove") == 0)) {
        const bool add = argv[0][0] == 'a';
        char origin[96];
        strlcpy(origin, argv[1], sizeof(origin));
        const size_t ol = strlen(origin);
        if (ol > 0 && origin[ol - 1] == '/') {
            origin[ol - 1] = '\0';     /* as copied from an address bar */
        }
        if (add && !valid_origin(origin)) {
            printf("web cors: an origin is scheme://host[:port], as https://editor.swagger.io; "
                   "https://*.example.com for its subdomains; or *\n");
            return 1;
        }
        /* Rebuild the list without it, then append it when adding */
        list[0] = '\0';
        bool found = false;
        for (const char *p = s_cors; *p; ) {
            while (*p == ' ') {
                p++;
            }
            const size_t n = strcspn(p, " ");
            if (n > 0) {
                if (n == strlen(origin) && strncasecmp(p, origin, n) == 0) {
                    found = true;
                } else {
                    snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%.*s", list[0] ? " " : "", (int)n, p);
                }
            }
            p += n;
        }
        if (!add && !found) {
            printf("web cors: %s is not on the list\n", origin);
            return 1;
        }
        if (add) {
            if (strlen(list) + strlen(origin) + 2 > sizeof(list)) {
                printf("web cors: the list is full (CONFIG_WEB_SERVER_CORS_MAX_LEN)\n");
                return 1;
            }
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", list[0] ? " " : "", origin);
        }
    } else {
        printf("usage: web cors [add <origin> | remove <origin> | reset | none]\n");
        return 1;
    }
    const esp_err_t err = nvs_put_str("cors", list);
    if (err != ESP_OK) {
        printf("web cors: cannot store it: %s\n", esp_err_to_name(err));
        return 1;
    }
    strlcpy(s_cors, list, sizeof(s_cors));
    print_cors();
    return 0;
}

static int web_cmd(int argc, char **argv)
{
    if (argc == 1) {
        print_web();
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        const bool on = strcmp(argv[1], "on") == 0;
        nvs_put_str("off", on ? NULL : "1");
        if (on) {
            const esp_err_t err = start_httpd();
            if (err != ESP_OK) {
                printf("web: cannot start: %s\n", esp_err_to_name(err));
                return 1;
            }
        } else {
            stop_httpd();
        }
        print_web();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "password") == 0) {
        const bool clear = strcmp(argv[2], "--clear") == 0;
        if (!clear && (strlen(argv[2]) < 4 || strlen(argv[2]) > 64)) {
            printf("web: a password is 4-64 characters\n");
            return 1;
        }
        const esp_err_t err = nvs_put_str("password", clear ? NULL : argv[2]);
        if (err != ESP_OK) {
            printf("web: cannot store it: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf(clear ? "password cleared: anyone on the network can update the board\n"
                     : "password set: changes over the web need it\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "cors") == 0) {
        return cors_cmd(argc - 2, argv + 2);
    }
    if (argc == 3 && strcmp(argv[1], "hostname") == 0) {
        const bool clear = strcmp(argv[2], "--clear") == 0;
        if (!clear && !valid_hostname(argv[2])) {
            printf("web: a hostname is 1-32 letters, digits and inner hyphens\n");
            return 1;
        }
        const esp_err_t err = nvs_put_str("hostname", clear ? NULL : argv[2]);
        if (err != ESP_OK) {
            printf("web: cannot store it: %s\n", esp_err_to_name(err));
            return 1;
        }
        load_hostname();
        apply_hostname();
        printf("hostname: %s (%s.local now; the router learns it at the next DHCP lease)\n", s_hostname, s_hostname);
        return 0;
    }
    printf("usage: web [on|off] | web password <password>|--clear | web hostname <name>|--clear\n"
           "       web cors [add <origin> | remove <origin> | reset | none]\n");
    return 1;
}

void web_server_register_commands(void)
{
    const esp_console_cmd_t cmd = {
        .command = "web",
        .help = "The web app and its API: where to reach it; on or off; the password updates need; the board's name; "
                "which other sites' pages may call the API (CORS)",
        .hint = "[on|off] | password <password>|--clear | hostname <name>|--clear | cors [add|remove <origin>|reset|none]",
        .func = web_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
