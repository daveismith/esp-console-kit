/*
 * api_core: routes, requests and replies without a transport. See api_core.h.
 *
 * Tables are added at start, from one task, before anything serves them; after that they are
 * only read, so nothing here is locked.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "api_core.h"

static const char *TAG = "api_core";

#define MAX_TABLES 16

static struct {
    const api_route_t *routes;
    size_t n;
} s_tables[MAX_TABLES];
static size_t s_n_tables;
static void (*s_watch)(const api_route_t *route);

static const char *const METHODS[] = { "GET", "PUT", "POST", "PATCH", "DELETE" };

esp_err_t api_add_routes(const api_route_t *routes, size_t n)
{
    if (s_n_tables >= MAX_TABLES) {
        ESP_LOGE(TAG, "no room for another table of routes");
        return ESP_ERR_NO_MEM;
    }
    s_tables[s_n_tables].routes = routes;
    s_tables[s_n_tables].n = n;
    s_n_tables++;
    for (size_t i = 0; s_watch != NULL && i < n; i++) {
        s_watch(&routes[i]);
    }
    return ESP_OK;
}

void api_watch_routes(void (*fn)(const api_route_t *route))
{
    s_watch = fn;
    for (size_t i = 0; fn != NULL; i++) {
        const api_route_t *r = api_route_at(i);
        if (r == NULL) {
            break;
        }
        fn(r);
    }
}

const api_route_t *api_route_at(size_t i)
{
    for (size_t t = 0; t < s_n_tables; t++) {
        if (i < s_tables[t].n) {
            return &s_tables[t].routes[i];
        }
        i -= s_tables[t].n;
    }
    return NULL;
}

const api_route_t *api_find(api_method_t method, const char *path, bool *path_known)
{
    bool known = false;
    const api_route_t *found = NULL;
    for (size_t i = 0; found == NULL; i++) {
        const api_route_t *r = api_route_at(i);
        if (r == NULL) {
            break;
        }
        if (strcmp(r->path, path) == 0) {
            known = true;
            if (r->method == method) {
                found = r;
            }
        }
    }
    if (path_known != NULL) {
        *path_known = known;
    }
    return found;
}

const char *api_method_name(api_method_t m)
{
    return (unsigned)m < sizeof(METHODS) / sizeof(METHODS[0]) ? METHODS[m] : "?";
}

bool api_method_of(const char *name, api_method_t *out)
{
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (strcmp(name, METHODS[i]) == 0) {
            *out = (api_method_t)i;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ replies */

api_reply_t api_json(int status, cJSON *body)
{
    return (api_reply_t){ .status = status, .body = body };
}

api_reply_t api_no_content(void)
{
    return (api_reply_t){ .status = 204, .body = NULL };
}

api_reply_t api_error(int status, const char *code, const char *fmt, ...)
{
    char message[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", message);
    return api_json(status, o);
}

/* ------------------------------------------------------------------ requests */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `in` (up to `in_len` bytes) as sent -- %2F, + -- to what it says, cut short to fit. %00 ends it. */
static void url_decode(const char *in, size_t in_len, char *out, size_t out_len)
{
    const char *end = in + in_len;
    size_t n = 0;
    while (in < end && n + 1 < out_len) {
        char c = *in++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && in + 1 < end && hex_value(in[0]) >= 0 && hex_value(in[1]) >= 0) {
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

bool api_query(const api_req_t *req, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    const size_t klen = strlen(key);
    for (const char *p = req->query; p != NULL && *p; ) {
        const char *amp = strchr(p, '&');
        const size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len >= klen && strncmp(p, key, klen) == 0 && (len == klen || p[klen] == '=')) {
            if (len > klen) {
                url_decode(p + klen + 1, len - klen - 1, out, out_len);
            }
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

bool api_query_bool(const api_req_t *req, const char *key, bool dflt)
{
    char v[8];
    if (!api_query(req, key, v, sizeof(v))) {
        return dflt;
    }
    return v[0] == '\0' || strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0;
}

bool api_only_keys(const cJSON *body, const char *const *keys, const char **bad)
{
    for (const cJSON *k = body != NULL ? body->child : NULL; k != NULL; k = k->next) {
        bool ok = false;
        for (size_t i = 0; keys[i] != NULL && !ok; i++) {
            ok = strcmp(k->string, keys[i]) == 0;
        }
        if (!ok) {
            *bad = k->string;
            return false;
        }
    }
    return true;
}
