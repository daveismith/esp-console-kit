/*
 * web_ota: the /api/v1/ota routes. See web_ota.h, and the OpenAPI description the application
 * serves at /api/v1/openapi.json.
 *
 * An upload runs on a task of its own (an async request), so the server keeps answering --
 * anyone can watch GET /api/v1/ota/image while an image arrives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ota_core.h"
#include "ota_pull.h"
#include "web_ota.h"
#include "web_server.h"

static const char *TAG = "web_ota";

#define CHUNK 4096
#define MAX_SLOTS 4

/* ------------------------------------------------------------------ JSON */

static const char *session_state(const ota_core_session_t *s)
{
    if (s->state == OTA_CORE_RECEIVING && s->source == OTA_CORE_SRC_PULL) {
        return "downloading";
    }
    return ota_core_state_name(s->state);
}

static cJSON *session_json(void)
{
    ota_core_session_t s;
    ota_core_get(&s);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", session_state(&s));
    cJSON_AddStringToObject(o, "source", ota_core_source_name(s.source));
    if (s.slot[0]) {
        cJSON_AddStringToObject(o, "slot", s.slot);
    } else {
        cJSON_AddNullToObject(o, "slot");
    }
    if (s.url[0]) {
        cJSON_AddStringToObject(o, "url", s.url);
    } else {
        cJSON_AddNullToObject(o, "url");
    }
    cJSON_AddNumberToObject(o, "bytes", (double)s.bytes);
    cJSON_AddNumberToObject(o, "total", (double)s.total);
    if (s.sha256[0]) {
        cJSON_AddStringToObject(o, "sha256", s.sha256);
    } else {
        cJSON_AddNullToObject(o, "sha256");
    }
    if (s.have_app) {
        cJSON_AddItemToObject(o, "app", web_app_desc_json(&s.app));
    } else {
        cJSON_AddNullToObject(o, "app");
    }
    cJSON_AddBoolToObject(o, "boots_next", s.state == OTA_CORE_ACTIVE);
    cJSON_AddBoolToObject(o, "dry_run", s.dry_run);
    if (s.error[0]) {
        cJSON_AddStringToObject(o, "error", s.error);
    } else {
        cJSON_AddNullToObject(o, "error");
    }
    const int64_t end = s.finished_us ? s.finished_us : esp_timer_get_time();
    cJSON_AddNumberToObject(o, "elapsed_ms", s.started_us ? (double)((end - s.started_us) / 1000) : 0);
    return o;
}

static cJSON *slots_json(void)
{
    ota_core_slot_t slots[MAX_SLOTS];
    const size_t n = ota_core_slots(slots, MAX_SLOTS);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        const ota_core_slot_t *s = &slots[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", s->label);
        cJSON_AddNumberToObject(o, "address", s->address);
        cJSON_AddNumberToObject(o, "size", s->size);
        cJSON_AddBoolToObject(o, "running", s->running);
        cJSON_AddBoolToObject(o, "boots", s->boots);
        cJSON_AddBoolToObject(o, "next", s->next);
        if (s->have_state) {
            cJSON_AddStringToObject(o, "state", ota_core_img_state_key(s->state));
        } else {
            cJSON_AddNullToObject(o, "state");
        }
        if (s->have_app) {
            cJSON_AddItemToObject(o, "app", web_app_desc_json(&s->app));
        } else {
            cJSON_AddNullToObject(o, "app");
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static bool station_online(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    return sta != NULL && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
           ip.ip.addr != 0;
}

/* ------------------------------------------------------------------ GET /ota, /ota/image */

static esp_err_t ota_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "rollback", CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE);
    cJSON_AddItemToObject(root, "slots", slots_json());
    cJSON_AddItemToObject(root, "session", session_json());
    cJSON *pull = cJSON_AddObjectToObject(root, "pull");
    cJSON_AddBoolToObject(pull, "available", ota_core_puller() != NULL);
    cJSON_AddBoolToObject(pull, "channels", ota_pull_has_channels());
    cJSON_AddBoolToObject(pull, "online", station_online());
    return web_send_json(req, 200, root);
}

static esp_err_t image_get(httpd_req_t *req)
{
    return web_send_json(req, 200, session_json());
}

/* ------------------------------------------------------------------ PUT /ota/image */

typedef struct {
    httpd_req_t *req;
    char sha256[65];
    bool activate;
    bool reboot;
} upload_job_t;

static int status_for(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_SIZE:        return 413;
    case ESP_ERR_INVALID_STATE:       return 409;
    case ESP_ERR_OTA_VALIDATE_FAILED:
    case ESP_ERR_INVALID_CRC:         return 422;
    default:                          return 500;
    }
}

static const char *code_for(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_SIZE:        return "bad_size";
    case ESP_ERR_INVALID_STATE:       return "cancelled";
    case ESP_ERR_INVALID_CRC:         return "sha256_mismatch";
    case ESP_ERR_OTA_VALIDATE_FAILED: return "invalid_image";
    default:                          return "failed";
    }
}

static esp_err_t upload_reply(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return web_send_json(req, 200, session_json());
    }
    ota_core_session_t s;
    ota_core_get(&s);
    return web_send_error(req, status_for(err), code_for(err), "%s", s.error[0] ? s.error : esp_err_to_name(err));
}

static void upload_run(upload_job_t *job)
{
    httpd_req_t *req = job->req;
    uint8_t *buf = malloc(CHUNK);
    esp_err_t err = buf != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    size_t left = req->content_len;
    int timeouts = 0;
    if (err != ESP_OK) {
        ota_core_fail("out of memory");
    }
    while (err == ESP_OK && left > 0) {
        const int n = httpd_req_recv(req, (char *)buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            ota_core_fail("the upload stopped after %u of %u bytes", (unsigned)(req->content_len - left),
                          (unsigned)req->content_len);
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = ota_core_write(buf, n);
        left -= n;
    }
    free(buf);
    if (err == ESP_OK) {
        err = ota_core_finish(job->sha256[0] ? job->sha256 : NULL);
    }
    if (err == ESP_OK && job->activate) {
        err = ota_core_activate(NULL);
        if (err == ESP_OK && job->reboot) {
            ota_core_restart_after(1500);
        }
    }
    if (err == ESP_FAIL) {
        return;             /* the connection is gone: nobody to tell */
    }
    upload_reply(req, err);
}

static void upload_task(void *arg)
{
    upload_job_t *job = arg;
    upload_run(job);
    httpd_req_async_handler_complete(job->req);
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t image_put(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return web_send_error(req, 411, "length_required", "send the image as the body, with a Content-Length");
    }
    upload_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return httpd_resp_send_500(req);
    }
    web_query(req, "sha256", job->sha256, sizeof(job->sha256));
    if (job->sha256[0] != '\0' && strlen(job->sha256) != 64) {
        free(job);
        return web_send_error(req, 400, "bad_sha256", "sha256 is 64 hex digits");
    }
    job->activate = web_query_bool(req, "activate", false);
    job->reboot = web_query_bool(req, "reboot", false);
    const bool force = web_query_bool(req, "force", false);

    const esp_err_t err = ota_core_begin(OTA_CORE_SRC_UPLOAD, req->content_len, NULL,
                                         force ? OTA_CORE_ANY_PROJECT : 0);
    if (err != ESP_OK) {
        free(job);
        if (err == ESP_ERR_INVALID_STATE) {
            return web_send_error(req, 409, "busy", "another update is in progress");
        }
        if (err == ESP_ERR_INVALID_SIZE) {
            const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
            return web_send_error(req, 413, "too_large", "%u bytes will not fit the %u KB slot",
                                  (unsigned)req->content_len, p ? (unsigned)(p->size / 1024) : 0);
        }
        ota_core_session_t s;
        ota_core_get(&s);
        return web_send_error(req, 500, "failed", "%s", s.error[0] ? s.error : esp_err_to_name(err));
    }

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) == ESP_OK) {
        job->req = copy;
        if (xTaskCreate(upload_task, "ota_upload", CONFIG_WEB_SERVER_UPLOAD_STACK_SIZE, job, 5, NULL) == pdPASS) {
            return ESP_OK;
        }
        httpd_req_async_handler_complete(copy);
    }
    /* No task: receive it here, holding up the server until it is done */
    job->req = req;
    upload_run(job);
    free(job);
    return ESP_OK;
}

/* ------------------------------------------------------------------ DELETE /ota/image */

static esp_err_t image_delete(httpd_req_t *req)
{
    const esp_err_t err = ota_core_discard();
    if (err != ESP_OK) {
        ota_core_session_t s;
        ota_core_get(&s);
        return web_send_error(req, 409, "cannot_discard", s.state == OTA_CORE_ACTIVE
            ? "it is already the boot image; activate the running slot to go back"
            : "it is being verified; try again in a moment");
    }
    return web_send_json(req, 200, session_json());
}

/* ------------------------------------------------------------------ POST /ota/activate */

static bool json_bool(const cJSON *body, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItem(body, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static const char *json_str(const cJSON *body, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(body, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static esp_err_t activate_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 512);
    if (body == NULL) {
        return ESP_OK;
    }
    const char *slot = json_str(body, "slot");
    const bool reboot = json_bool(body, "reboot", true);
    const esp_err_t err = ota_core_activate(slot);
    esp_err_t sent;
    if (err == ESP_ERR_INVALID_STATE) {
        sent = web_send_error(req, 409, slot ? "busy" : "nothing_staged",
                              slot ? "an update is writing that slot" : "nothing is staged; name a slot");
    } else if (err == ESP_ERR_NOT_FOUND) {
        sent = web_send_error(req, 404, "no_such_slot", "no application slot called %s", slot);
    } else if (err != ESP_OK) {
        sent = web_send_error(req, 422, "invalid_image", "%s holds no good image (%s)",
                              slot ? slot : "the staged slot", esp_err_to_name(err));
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "session", session_json());
        if (reboot) {
            ota_core_restart_after(1000);
            cJSON_AddNumberToObject(root, "restart_in_ms", 1000);
        } else {
            cJSON_AddNullToObject(root, "restart_in_ms");
        }
        sent = web_send_json(req, 202, root);
    }
    cJSON_Delete(body);
    return sent;
}

/* ------------------------------------------------------------------ POST /ota/pull */

static esp_err_t pull_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 1024);
    if (body == NULL) {
        return ESP_OK;
    }
    const char *url = json_str(body, "url");
    const char *channel = json_str(body, "channel");
    esp_err_t sent;
    if ((url == NULL) == (channel == NULL)) {
        sent = web_send_error(req, 400, "bad_request", "give either `url` or `channel`");
    } else if (ota_core_puller() == NULL) {
        sent = web_send_error(req, 503, "unavailable", "this firmware cannot pull images");
    } else if (!station_online()) {
        sent = web_send_error(req, 503, "offline", "the board has no network connection to download from");
    } else {
        const ota_core_pull_req_t pr = {
            .what = url ? url : channel,
            .sha256 = json_str(body, "sha256"),
            .activate = json_bool(body, "activate", false),
            .reboot = json_bool(body, "reboot", false),
            .any_project = json_bool(body, "force", false),
        };
        char why[128] = "";
        const esp_err_t err = ota_core_puller()(&pr, why, sizeof(why));
        if (err == ESP_OK) {
            sent = web_send_json(req, 202, session_json());
        } else if (err == ESP_ERR_INVALID_STATE) {
            sent = web_send_error(req, 409, "busy", "%s", why);
        } else if (err == ESP_ERR_INVALID_ARG) {
            sent = web_send_error(req, 400, "bad_request", "%s", why);
        } else {
            sent = web_send_error(req, 500, "failed", "%s", why);
        }
    }
    cJSON_Delete(body);
    return sent;
}

/* ------------------------------------------------------------------ GET /ota/check */

int web_ota_compare_versions(const char *a, const char *b)
{
    unsigned x[4], y[4];
    char tail;
    /* vX.Y.Z, or vX.Y.Z-rcN; a release sorts after its own release candidates */
    const int nx = sscanf(a, "v%u.%u.%u-rc%u%c", &x[0], &x[1], &x[2], &x[3], &tail);
    const int ny = sscanf(b, "v%u.%u.%u-rc%u%c", &y[0], &y[1], &y[2], &y[3], &tail);
    char check[40];
    if (nx == 3) {
        snprintf(check, sizeof(check), "v%u.%u.%u", x[0], x[1], x[2]);
        if (strcmp(check, a) != 0) {
            return 2;       /* v1.0.0-6-gabc: a development build */
        }
        x[3] = UINT32_MAX;
    } else if (nx != 4) {
        return 2;
    }
    if (ny == 3) {
        snprintf(check, sizeof(check), "v%u.%u.%u", y[0], y[1], y[2]);
        if (strcmp(check, b) != 0) {
            return 2;
        }
        y[3] = UINT32_MAX;
    } else if (ny != 4) {
        return 2;
    }
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

/* One answer per channel for a few minutes: a check costs a TLS handshake or two, and holds up
 * the server while it runs. */
static struct {
    char what[64];
    int64_t at_us;
    ota_pull_target_t target;
} s_check_cache;

#define CHECK_CACHE_US (5LL * 60 * 1000000)

/* Resolving means TLS, which wants more stack than the server's task has: it runs on a task
 * of the download's size, and the handler waits for it. */
typedef struct {
    const char *what;
    ota_pull_target_t *target;
    char why[128];
    esp_err_t err;
    TaskHandle_t waiter;
} resolve_job_t;

static void resolve_task(void *arg)
{
    resolve_job_t *job = arg;
    job->err = ota_pull_resolve(job->what, job->target, job->why, sizeof(job->why));
    xTaskNotifyGive(job->waiter);
    vTaskDelete(NULL);
}

static esp_err_t resolve_on_big_stack(const char *what, ota_pull_target_t *t, char *why, size_t why_len)
{
    resolve_job_t job = { .what = what, .target = t, .err = ESP_FAIL, .waiter = xTaskGetCurrentTaskHandle() };
    if (xTaskCreate(resolve_task, "ota_check", CONFIG_WEB_SERVER_PULL_STACK_SIZE, &job, 5, NULL) != pdPASS) {
        snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* its own timeouts bound it: 15 s a fetch */
    strlcpy(why, job.why, why_len);
    return job.err;
}

static esp_err_t check_get(httpd_req_t *req)
{
    char what[256];
    if (!web_query(req, "url", what, sizeof(what)) || what[0] == '\0') {
        if (!web_query(req, "channel", what, sizeof(what)) || what[0] == '\0') {
            strlcpy(what, "latest", sizeof(what));
        }
    }
    const bool fresh = web_query_bool(req, "refresh", false);
    if (!station_online()) {
        return web_send_error(req, 503, "offline", "the board has no network connection to look from");
    }
    ota_pull_target_t t;
    const int64_t now = esp_timer_get_time();
    if (!fresh && strcmp(s_check_cache.what, what) == 0 && s_check_cache.at_us &&
        now - s_check_cache.at_us < CHECK_CACHE_US) {
        t = s_check_cache.target;
    } else {
        char why[128] = "";
        const esp_err_t err = resolve_on_big_stack(what, &t, why, sizeof(why));
        if (err == ESP_ERR_INVALID_ARG) {
            return web_send_error(req, 400, "bad_request", "%s", why);
        }
        if (err == ESP_ERR_NOT_FOUND) {
            return web_send_error(req, 404, "no_firmware", "%s", why);
        }
        if (err != ESP_OK) {
            return web_send_error(req, 503, "unreachable", "%s", why);
        }
        strlcpy(s_check_cache.what, what, sizeof(s_check_cache.what));
        s_check_cache.at_us = now;
        s_check_cache.target = t;
    }

    const char *current = esp_app_get_description()->version;
    cJSON *root = cJSON_CreateObject();
    if (t.channel[0]) {
        cJSON_AddStringToObject(root, "channel", t.channel);
    } else {
        cJSON_AddNullToObject(root, "channel");
    }
    cJSON_AddStringToObject(root, "current", current);
    if (t.version[0]) {
        cJSON_AddStringToObject(root, "version", t.version);
    } else {
        cJSON_AddNullToObject(root, "version");
    }
    const int order = t.version[0] ? web_ota_compare_versions(current, t.version) : 2;
    if (order == 2) {
        cJSON_AddNullToObject(root, "newer");
    } else {
        cJSON_AddBoolToObject(root, "newer", order < 0);
    }
    cJSON_AddStringToObject(root, "image_url", t.image_url);
    if (t.release_url[0]) {
        cJSON_AddStringToObject(root, "release_url", t.release_url);
    } else {
        cJSON_AddNullToObject(root, "release_url");
    }
    cJSON_AddNumberToObject(root, "size", (double)t.size);
    if (t.sha256[0]) {
        cJSON_AddStringToObject(root, "sha256", t.sha256);
    } else {
        cJSON_AddNullToObject(root, "sha256");
    }
    cJSON_AddNumberToObject(root, "checked_s_ago", (double)((now - s_check_cache.at_us) / 1000000));
    return web_send_json(req, 200, root);
}

/* ------------------------------------------------------------------ registration */

esp_err_t web_ota_register(void)
{
    ota_core_set_puller(ota_pull_start);
    web_server_add_feature("ota");
    esp_err_t err = ESP_OK;
    err |= web_register("/api/v1/ota", HTTP_GET, ota_get, 0);
    err |= web_register("/api/v1/ota/image", HTTP_GET, image_get, 0);
    err |= web_register("/api/v1/ota/image", HTTP_PUT, image_put, WEB_AUTH);
    err |= web_register("/api/v1/ota/image", HTTP_DELETE, image_delete, WEB_AUTH);
    err |= web_register("/api/v1/ota/activate", HTTP_POST, activate_post, WEB_AUTH);
    err |= web_register("/api/v1/ota/pull", HTTP_POST, pull_post, WEB_AUTH);
    err |= web_register("/api/v1/ota/check", HTTP_GET, check_get, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not every route registered");
        return ESP_FAIL;
    }
    return ESP_OK;
}
