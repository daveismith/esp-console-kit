/*
 * web_server -- the board's HTTP server: a JSON API under /api/v1 and an application's web
 * pages, on every interface (the station's address, `<hostname>.local`, and the access point
 * at 192.168.4.1). See web_server.c.
 *
 * The application passes its pages as embedded assets, and registers API routes with
 * web_register(). Components add their own: web_ota.c registers the /api/v1/ota routes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A file served as-is. `path` is the URL path ("/index.html"; "/" serves "/index.html"). */
typedef struct {
    const char *path;
    const char *type;           /* Content-Type */
    const uint8_t *data;
    size_t len;
    bool gzip;                  /* `data` is gzipped: served with Content-Encoding: gzip */
} web_asset_t;

typedef struct {
    const char *name_prefix;    /* the default hostname is <prefix>-xxxx (station MAC) */
    const char *product;        /* for mDNS: the service's instance name */
    const web_asset_t *assets;
    size_t n_assets;
} web_server_config_t;

/* Route flags */
#define WEB_AUTH        (1u << 0)   /* needs the password, when one is set -- whatever the method */

typedef esp_err_t (*web_handler_t)(httpd_req_t *req);

/*
 * Start the server (unless `web off` stored it off), mDNS, and the routes registered so
 * far. Needs NVS, the netifs (wifi_bringup()) and the default event loop.
 */
esp_err_t web_server_start(const web_server_config_t *cfg);
bool web_server_running(void);

/* An API route. Before or after the start; at most CONFIG_WEB_SERVER_MAX_ROUTES in all.
 * Mutating routes (PUT, POST, PATCH, DELETE) are always checked against cross-site requests,
 * and a POST or PATCH must be JSON. A WEB_AUTH route -- of any method, a GET that reads
 * something private too -- needs the password, when one is set. */
esp_err_t web_register(const char *uri, httpd_method_t method, web_handler_t handler, unsigned flags);

/* What /api/v1/info lists under `features`: the pages the web app shows. */
void web_server_add_feature(const char *name);

/* `web` */
void web_server_register_commands(void);

/* The name the board answers to: `<name>.local`, and its DHCP hostname. */
const char *web_server_hostname(void);

/* ---- for handlers ---- */

/* Send `root` as JSON with `status` (200, 202, ...), and free it. */
esp_err_t web_send_json(httpd_req_t *req, int status, cJSON *root);

/* {"error": code, "message": ...} with `status`. */
esp_err_t web_send_error(httpd_req_t *req, int status, const char *code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/*
 * The request's JSON body: an object, or an empty object for no body. NULL after sending the
 * error itself (too large, not JSON, not an object): the handler just returns ESP_OK.
 */
cJSON *web_read_json(httpd_req_t *req, size_t max_len);

/* Query-string parameters, URL-decoded (%XX, and + for a space). `out` is empty and false
 * returned when absent; a value too long for `out` is cut short. */
bool web_query(httpd_req_t *req, const char *key, char *out, size_t out_len);
bool web_query_bool(httpd_req_t *req, const char *key, bool dflt);

/* Whether the request came in on the access point, not the station. */
bool web_req_via_ap(httpd_req_t *req);

/* The station, as /api/v1/info gives it: enabled, connected; ssid, ip, rssi, channel, ipv6 when
 * connected. */
cJSON *web_sta_json(void);

/* JSON of an esp_app_desc_t: project, version, date, time, idf, elf_sha256. */
cJSON *web_app_desc_json(const esp_app_desc_t *d);

/* Send the embedded asset at `path` (200) if there is one; ESP_ERR_NOT_FOUND otherwise, with
 * nothing sent. */
esp_err_t web_send_asset(httpd_req_t *req, const char *path);

/* ---- long operations ----
 *
 * An upload, a download, a copy, a hash, a scan: anything that takes seconds runs on a task of
 * its own with an async copy of the request, so the server keeps answering meanwhile. One runs
 * at a time -- and none while a firmware update is being received or verified -- so a second
 * gets 409 busy. `what` names the one running, for that reply: "a file upload". */

typedef void (*web_job_fn_t)(httpd_req_t *req, void *ctx);

/* Take the one slot. When it is taken (or an update is running), false -- after sending the 409
 * itself if `req` is not NULL. `what` must be a string literal. */
bool web_job_claim(httpd_req_t *req, const char *what);

/* Run `fn(req, ctx)` on a task of its own (CONFIG_WEB_SERVER_UPLOAD_STACK_SIZE), with the slot
 * claimed: `fn` sends the reply and frees `ctx`, and the slot is released after it. When no
 * task can be made, `fn` runs here instead, holding up the server until it is done. */
esp_err_t web_job_run(httpd_req_t *req, web_job_fn_t fn, void *ctx);

/* Claim, then run. ESP_ERR_INVALID_STATE, with the 409 sent, when busy: `ctx` is still the
 * caller's to free. The handler returns ESP_OK either way. */
esp_err_t web_job_start(httpd_req_t *req, const char *what, web_job_fn_t fn, void *ctx);

/* Give back a slot claimed without running a job (the claim was for an operation that then
 * failed to start). */
void web_job_release(void);

/* What is running now, or NULL. */
const char *web_job_running(void);

#ifdef __cplusplus
}
#endif
