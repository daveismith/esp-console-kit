/*
 * api_core -- the JSON API without its transport. A route is a method, a path and a function
 * from a request (a JSON body, a query string) to a reply (a status, a JSON body). The web server
 * serves every route over HTTP (web_server); a host link serves the ones marked API_LINK over a
 * wire. Nothing here is HTTP. See api_core.c.
 *
 * Declare routes in a table with API_ROUTE() -- tools/check_api_docs.py reads those, as it reads
 * web_register() calls -- and add the table with api_add_routes().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { API_GET, API_PUT, API_POST, API_PATCH, API_DELETE } api_method_t;

/* Route flags */
#define API_LINK        (1u << 0)   /* served over the host link too */
#define API_PRIVATE     (1u << 1)   /* a GET that needs the password, when one is set (changes always do) */
#define API_NO_HTTP     (1u << 2)   /* the host link's only */

typedef struct {
    const char *query;      /* the raw query string ("a=1&b=%2Fx"), or NULL */
    const cJSON *body;      /* an object: empty when the request had none */
    const char *via;        /* "sta", "ap" or "link" */
} api_req_t;

typedef struct {
    int status;             /* 200, 201, 202, 204, 400, ... */
    cJSON *body;            /* the reply, owned by whoever sends it; NULL with 204 */
} api_reply_t;

typedef api_reply_t (*api_fn_t)(const api_req_t *req);

typedef struct {
    api_method_t method;
    const char *path;       /* "/api/v1/screen" */
    api_fn_t fn;
    uint16_t body_max;      /* the largest JSON body it takes; 0 for none */
    uint8_t flags;
} api_route_t;

#define API_ROUTE(method, path, fn, body_max, flags) { (method), (path), (fn), (body_max), (flags) }

/* Add a table (it must outlive the program: a static const array). Anyone watching -- the web
 * server -- is told of each route, now and for tables added later. */
esp_err_t api_add_routes(const api_route_t *routes, size_t n);

/* Be told of every route: those added so far at once, and each added later. */
void api_watch_routes(void (*fn)(const api_route_t *route));

/* The route for `method` and `path` (no query), or NULL; `path_known` says whether any method
 * has that path (405 rather than 404). */
const api_route_t *api_find(api_method_t method, const char *path, bool *path_known);

/* Each route, in the order added: NULL past the last. */
const api_route_t *api_route_at(size_t i);

const char *api_method_name(api_method_t m);
bool api_method_of(const char *name, api_method_t *out);

/* ---- for route functions ---- */

api_reply_t api_json(int status, cJSON *body);
api_reply_t api_no_content(void);
/* {"error": code, "message": ...} */
api_reply_t api_error(int status, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* The query's `key`, URL-decoded. False, and `out` empty, when absent; a bare key ("?all") is
 * present and empty. */
bool api_query(const api_req_t *req, const char *key, char *out, size_t out_len);
bool api_query_bool(const api_req_t *req, const char *key, bool dflt);

/* The body's keys are all among `keys` (NULL-terminated): false with the first that isn't. */
bool api_only_keys(const cJSON *body, const char *const *keys, const char **bad);

#ifdef __cplusplus
}
#endif
