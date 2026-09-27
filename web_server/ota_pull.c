/*
 * ota_pull: the board downloads its own update. See ota_pull.h.
 *
 * Redirects are followed by hand, not by esp_http_client, so that one from https to plain
 * http is refused whatever the IDF version: it would let anyone on the path substitute the
 * image. (A manifest's SHA-256 would still catch that, but a bare image URL has nothing else
 * protecting it.) IDF 6.1's esp_http_client_set_redirection() refuses it too.
 * Certificates are checked against ESP-IDF's bundle of the common root CAs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_pull.h"
#include "web_server.h"

static const char *TAG = "ota_pull";

#define CHUNK 4096

static ota_pull_resolver_t s_resolver;

void ota_pull_set_resolver(ota_pull_resolver_t resolver)
{
    s_resolver = resolver;
}

bool ota_pull_has_channels(void)
{
    return s_resolver != NULL;
}

static bool is_https(const char *url)
{
    return strncasecmp(url, "https://", 8) == 0;
}

static esp_http_client_handle_t client_for(const char *url)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "esp-console-kit ota_pull",
    };
    return esp_http_client_init(&cfg);
}

/* Open the request and read the headers, following redirects. On success the body is ready
 * to read and `*len` is its Content-Length (-1 if not given). */
static esp_err_t open_following(esp_http_client_handle_t c, int64_t *len, char *why, size_t why_len)
{
    char url[320];
    esp_http_client_get_url(c, url, sizeof(url));
    const bool https = is_https(url);
    for (int hop = 0; hop < 6; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            snprintf(why, why_len, "cannot connect for %.80s: %s", url, esp_err_to_name(err));
            return err;
        }
        const int64_t n = esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            err = esp_http_client_set_redirection(c);
            if (err == ESP_ERR_HTTP_REDIRECT_DOWNGRADE) {
                snprintf(why, why_len, "refused a redirect from https to plain http (from %.80s)", url);
                esp_http_client_close(c);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (err != ESP_OK) {
                snprintf(why, why_len, "a redirect (%d) with no usable Location", status);
                esp_http_client_close(c);
                return ESP_ERR_INVALID_RESPONSE;
            }
            esp_http_client_flush_response(c, NULL);
            esp_http_client_close(c);
            esp_http_client_get_url(c, url, sizeof(url));
            if (https && !is_https(url)) {
                snprintf(why, why_len, "refused a redirect from https to plain http (%.80s)", url);
                return ESP_ERR_INVALID_RESPONSE;
            }
            continue;
        }
        if (status != 200) {
            snprintf(why, why_len, "HTTP %d for %.100s", status, url);
            esp_http_client_close(c);
            return status == 404 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
        }
        *len = esp_http_client_is_chunked_response(c) ? -1 : n;
        return ESP_OK;
    }
    snprintf(why, why_len, "too many redirects");
    esp_http_client_close(c);
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ota_pull_fetch(const char *url, char **body, size_t max, char *why, size_t why_len)
{
    *body = NULL;
    esp_http_client_handle_t c = client_for(url);
    if (c == NULL) {
        snprintf(why, why_len, "not a URL: %.100s", url);
        return ESP_ERR_INVALID_ARG;
    }
    int64_t len = -1;
    esp_err_t err = open_following(c, &len, why, why_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    if (len > (int64_t)max) {
        snprintf(why, why_len, "%.80s is over %u bytes", url, (unsigned)max);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(max + 1);
    size_t got = 0;
    while (buf != NULL && got < max) {
        const int n = esp_http_client_read(c, buf + got, max - got);
        if (n < 0) {
            snprintf(why, why_len, "reading %.80s: %s", url, esp_err_to_name(-n));
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    esp_http_client_cleanup(c);
    if (buf == NULL) {
        snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[got] = '\0';
    *body = buf;
    return ESP_OK;
}

/* `path` relative to the document at `base` */
static void join_url(const char *base, const char *path, char *out, size_t out_len)
{
    if (strstr(path, "://") != NULL) {
        strlcpy(out, path, out_len);
        return;
    }
    const char *scheme_end = strstr(base, "://");
    const char *host_end = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    if (path[0] == '/') {
        const int n = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, out_len, "%.*s%s", n, base, path);
        return;
    }
    const char *q = strchr(base, '?');
    const size_t base_len = q ? (size_t)(q - base) : strlen(base);
    const char *slash = NULL;
    for (const char *p = base; p < base + base_len; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    const int n = slash && (!host_end || slash >= host_end) ? (int)(slash - base + 1) : (int)base_len;
    snprintf(out, out_len, "%.*s%s%s", n, base, slash && (!host_end || slash >= host_end) ? "" : "/", path);
}

static bool ends_with_json(const char *url)
{
    const char *q = strchr(url, '?');
    const size_t n = q ? (size_t)(q - url) : strlen(url);
    return n >= 5 && strncasecmp(url + n - 5, ".json", 5) == 0;
}

static void copy_str(const cJSON *o, const char *key, char *out, size_t len)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsString(v)) {
        strlcpy(out, v->valuestring, len);
    }
}

static esp_err_t read_manifest(const char *url, ota_pull_target_t *t, char *why, size_t why_len)
{
    char *text = NULL;
    esp_err_t err = ota_pull_fetch(url, &text, 16 * 1024, why, why_len);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *m = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        snprintf(why, why_len, "the manifest is not JSON: %.80s", url);
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_str(m, "project", t->project, sizeof(t->project));
    copy_str(m, "version", t->version, sizeof(t->version));
    copy_str(m, "release", t->release_url, sizeof(t->release_url));
    const cJSON *parts = cJSON_GetObjectItem(m, "parts");
    const cJSON *app = NULL;
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *role = cJSON_GetObjectItem(part, "role");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "app") == 0) {
            app = part;
            break;
        }
    }
    const cJSON *path = cJSON_GetObjectItem(app, "path");
    if (!cJSON_IsString(path)) {
        cJSON_Delete(m);
        snprintf(why, why_len, "the manifest lists no application image");
        return ESP_ERR_INVALID_RESPONSE;
    }
    join_url(url, path->valuestring, t->image_url, sizeof(t->image_url));
    copy_str(app, "sha256", t->sha256, sizeof(t->sha256));
    const cJSON *size = cJSON_GetObjectItem(app, "size");
    t->size = cJSON_IsNumber(size) && size->valuedouble > 0 ? (size_t)size->valuedouble : 0;
    cJSON_Delete(m);
    if (t->sha256[0] != '\0' && strlen(t->sha256) != 64) {
        snprintf(why, why_len, "the manifest's sha256 is malformed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ota_pull_resolve(const char *what, ota_pull_target_t *out, char *why, size_t why_len)
{
    memset(out, 0, sizeof(*out));
    if (what == NULL || what[0] == '\0') {
        snprintf(why, why_len, "nothing to pull");
        return ESP_ERR_INVALID_ARG;
    }
    char manifest[256] = "";
    if (strstr(what, "://") == NULL) {
        if (s_resolver == NULL) {
            snprintf(why, why_len, "`%.24s` is not a URL, and this firmware knows no release channels", what);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(out->channel, what, sizeof(out->channel));
        esp_err_t err = s_resolver(what, manifest, sizeof(manifest), why, why_len);
        if (err != ESP_OK) {
            return err;
        }
        err = read_manifest(manifest, out, why, why_len);
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(why, why_len, "the `%.24s` channel publishes no firmware: only releases do", what);
        }
        return err;
    } else if (strncasecmp(what, "http://", 7) != 0 && !is_https(what)) {
        snprintf(why, why_len, "only http:// and https:// URLs");
        return ESP_ERR_INVALID_ARG;
    } else if (ends_with_json(what)) {
        strlcpy(manifest, what, sizeof(manifest));
    } else {
        strlcpy(out->image_url, what, sizeof(out->image_url));
        return ESP_OK;
    }
    return read_manifest(manifest, out, why, why_len);
}

/* ------------------------------------------------------------------ the download */

typedef struct {
    char what[256];
    char sha256[65];
    bool activate;
    bool reboot;
    bool any_project;
} pull_job_t;

static void pull_task(void *arg)
{
    pull_job_t *job = arg;
    char why[128] = "";
    ota_pull_target_t t;
    uint8_t *buf = NULL;
    esp_http_client_handle_t c = NULL;

    if (ota_pull_resolve(job->what, &t, why, sizeof(why)) != ESP_OK) {
        ota_core_fail("%s", why);
        goto done;
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (!job->any_project && t.project[0] != '\0' && strcmp(t.project, self->project_name) != 0) {
        ota_core_fail("the release is for %s, this is %s", t.project, self->project_name);
        goto done;
    }
    ota_core_set_source(t.image_url, t.size);
    ESP_LOGI(TAG, "downloading %s%s%s", t.image_url, t.version[0] ? ", " : "", t.version);

    c = client_for(t.image_url);
    buf = malloc(CHUNK);
    if (c == NULL || buf == NULL) {
        ota_core_fail("out of memory");
        goto done;
    }
    int64_t len = -1;
    if (open_following(c, &len, why, sizeof(why)) != ESP_OK) {
        ota_core_fail("%s", why);
        goto done;
    }
    if (t.size && len >= 0 && (size_t)len != t.size) {
        ota_core_fail("the server offers %lld bytes, the manifest says %u", (long long)len, (unsigned)t.size);
        goto done;
    }
    if (!t.size && len > 0) {
        ota_core_set_source(NULL, (size_t)len);
    }
    size_t got = 0;
    for (;;) {
        const int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            ota_core_fail("the download failed after %u bytes: %s", (unsigned)got, esp_err_to_name(-n));
            goto done;
        }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(c)) {
                ota_core_fail("the download stopped after %u bytes", (unsigned)got);
                goto done;
            }
            break;
        }
        if (ota_core_write(buf, n) != ESP_OK) {
            goto done;      /* the session says why */
        }
        got += n;
    }
    esp_http_client_cleanup(c);
    c = NULL;

    if (ota_core_finish(job->sha256[0] ? job->sha256 : (t.sha256[0] ? t.sha256 : NULL)) != ESP_OK) {
        goto done;
    }
    if (job->activate && ota_core_activate(NULL) == ESP_OK && job->reboot) {
        ota_core_restart_after(1500);
    }

done:
    if (c != NULL) {
        esp_http_client_cleanup(c);
    }
    free(buf);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t ota_pull_start(const ota_core_pull_req_t *req, char *why, size_t why_len)
{
    if (req->what == NULL || req->what[0] == '\0' || strlen(req->what) >= sizeof(((pull_job_t *)0)->what)) {
        snprintf(why, why_len, "nothing to pull, or a URL over 255 characters");
        return ESP_ERR_INVALID_ARG;
    }
    if (req->sha256 != NULL && req->sha256[0] != '\0' && strlen(req->sha256) != 64) {
        snprintf(why, why_len, "sha256 is 64 hex digits");
        return ESP_ERR_INVALID_ARG;
    }
    const char *running = web_job_running();
    if (running != NULL) {
        snprintf(why, why_len, "%s is in progress; try again when it is done", running);
        return ESP_ERR_INVALID_STATE;
    }
    pull_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(job->what, req->what, sizeof(job->what));
    if (req->sha256 != NULL) {
        strlcpy(job->sha256, req->sha256, sizeof(job->sha256));
    }
    job->activate = req->activate;
    job->reboot = req->reboot;
    job->any_project = req->any_project;

    /* The session is claimed here, so a second pull or upload is refused at once */
    const esp_err_t err = ota_core_begin(OTA_CORE_SRC_PULL, 0, req->what,
                                         req->any_project ? OTA_CORE_ANY_PROJECT : 0);
    if (err != ESP_OK) {
        free(job);
        snprintf(why, why_len, "%s", err == ESP_ERR_INVALID_STATE ? "another update is in progress"
                                     : esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(pull_task, "ota_pull", CONFIG_WEB_SERVER_PULL_STACK_SIZE, job, 5, NULL) != pdPASS) {
        free(job);
        ota_core_fail("out of memory for the download task");
        snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
