/*
 * web_servo: the /api/v1/servos routes. See web_servo.h.
 *
 * A servo is named by its ident (`id`): in the query for its saved settings (PUT and DELETE
 * /servos/calibration, PUT /servos/policy), in the body for an action.
 */
#include <stdio.h>
#include <string.h>
#include "servo.h"
#include "web_servo.h"
#include "web_server.h"

#define ID_LEN 24

static void (*s_before_move)(void);

static cJSON *servo_json(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    const int gpio = servo_gpio(id);
    if (gpio >= 0) {
        cJSON_AddNumberToObject(o, "gpio", gpio);
    }
    uint16_t us = 0;
    servo_get_value_us(id, &us);
    cJSON_AddNumberToObject(o, "us", us);
    bool driven = false, released = false;
    servo_get_output(id, &driven, &released);
    cJSON_AddBoolToObject(o, "driven", driven);
    cJSON_AddBoolToObject(o, "released", released);

    servo_limits_t l = { 0 };
    servo_get_limits(id, &l);
    cJSON *lo = cJSON_AddObjectToObject(o, "limits");
    cJSON_AddNumberToObject(lo, "abs_min_us", l.abs_min_us);
    cJSON_AddNumberToObject(lo, "abs_max_us", l.abs_max_us);
    cJSON_AddNumberToObject(lo, "op_min_us", l.op_min_us);
    cJSON_AddNumberToObject(lo, "op_max_us", l.op_max_us);
    cJSON_AddBoolToObject(lo, "invert", l.invert);
    cJSON_AddBoolToObject(lo, "calibrated", l.calibrated);

    servo_drive_policy_t p = { 0 };
    servo_get_drive_policy(id, &p);
    cJSON *po = cJSON_AddObjectToObject(o, "policy");
    cJSON_AddStringToObject(po, "closed", p.drive_closed == SERVO_DRIVE_RELEASE ? "release" : "hold");
    cJSON_AddStringToObject(po, "open", p.drive_open == SERVO_DRIVE_RELEASE ? "release" : "hold");
    cJSON_AddStringToObject(po, "mid", p.drive_mid == SERVO_DRIVE_RELEASE ? "release" : "hold");
    cJSON_AddNumberToObject(po, "settle_ms", p.settle_ms);
    return o;
}

static cJSON *servo_list_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "servos");
    char id[ID_LEN];
    for (size_t i = 0; servo_ident(i, id, sizeof(id)); i++) {
        cJSON_AddItemToArray(arr, servo_json(id));
    }
    return root;
}

/* 404 naming the servos there are. */
static esp_err_t no_such_servo(httpd_req_t *req, const char *id)
{
    char names[96] = "";
    char one[ID_LEN];
    for (size_t i = 0; servo_ident(i, one, sizeof(one)); i++) {
        snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", i ? ", " : "", one);
    }
    return web_send_error(req, 404, "not_found", "no servo '%s' (there is: %s)", id, names[0] ? names : "none");
}

/* The query's `id`: false after replying. */
static bool query_id(httpd_req_t *req, char *id)
{
    if (!web_query(req, "id", id, ID_LEN) || id[0] == '\0') {
        web_send_error(req, 400, "bad_request", "give `id`, a servo as GET /api/v1/servos lists them");
        return false;
    }
    if (!servo_exists(id)) {
        no_such_servo(req, id);
        return false;
    }
    return true;
}

/* A body's `id`: false after replying. `all` allowed if asked. */
static bool body_id(httpd_req_t *req, const cJSON *body, char *id, bool all)
{
    const cJSON *v = cJSON_GetObjectItem(body, "id");
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= ID_LEN) {
        web_send_error(req, 400, "bad_request", "give `id`, a servo as GET /api/v1/servos lists them%s",
                       all ? ", or all" : "");
        return false;
    }
    strlcpy(id, v->valuestring, ID_LEN);
    if ((all && strcmp(id, "all") == 0) || servo_exists(id)) {
        return true;
    }
    no_such_servo(req, id);
    return false;
}

static bool only_keys(const cJSON *body, const char *const *keys, size_t n, const char **bad)
{
    for (const cJSON *k = body->child; k != NULL; k = k->next) {
        bool ok = false;
        for (size_t i = 0; i < n && !ok; i++) {
            ok = strcmp(k->string, keys[i]) == 0;
        }
        if (!ok) {
            *bad = k->string;
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ routes */

static esp_err_t servos_get(httpd_req_t *req)
{
    return web_send_json(req, 200, servo_list_json());
}

static esp_err_t move_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    char id[ID_LEN];
    static const char *const KEYS[] = { "id", "us", "percent" };
    const char *bad = NULL;
    const cJSON *us = cJSON_GetObjectItem(body, "us");
    const cJSON *pct = cJSON_GetObjectItem(body, "percent");
    esp_err_t sent = ESP_FAIL;
    if (!only_keys(body, KEYS, 3, &bad)) {
        sent = web_send_error(req, 400, "bad_request", "unknown field `%s`", bad);
    } else if (!body_id(req, body, id, false)) {
        sent = ESP_OK;
    } else if ((us == NULL) == (pct == NULL) || (us && !cJSON_IsNumber(us)) || (pct && !cJSON_IsNumber(pct))) {
        sent = web_send_error(req, 400, "bad_request", "give `us` or `percent`, a number");
    } else {
        servo_limits_t l;
        servo_get_limits(id, &l);
        if (us != NULL && (us->valuedouble < l.abs_min_us || us->valuedouble > l.abs_max_us)) {
            sent = web_send_error(req, 400, "bad_request", "`us` must be within %u..%u", l.abs_min_us, l.abs_max_us);
        } else if (pct != NULL && (pct->valuedouble < 0 || pct->valuedouble > 100)) {
            sent = web_send_error(req, 400, "bad_request", "`percent` is 0..100");
        } else {
            if (s_before_move != NULL) {
                s_before_move();
            }
            const bool ok = us != NULL ? servo_move_us(id, (uint16_t)us->valuedouble)
                                       : servo_set_percentage(id, (uint16_t)(pct->valuedouble * 10 + 0.5));
            servo_set_enable(id, true);
            sent = ok ? web_send_json(req, 200, servo_json(id))
                      : web_send_error(req, 500, "failed", "%s did not move", id);
        }
    }
    cJSON_Delete(body);
    return sent == ESP_FAIL ? ESP_OK : sent;
}

static esp_err_t release_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 128);
    if (body == NULL) {
        return ESP_OK;
    }
    char id[ID_LEN];
    const bool ok = body_id(req, body, id, true);
    cJSON_Delete(body);
    if (!ok) {
        return ESP_OK;
    }
    if (strcmp(id, "all") == 0) {
        char one[ID_LEN];
        for (size_t i = 0; servo_ident(i, one, sizeof(one)); i++) {
            servo_set_enable(one, false);
        }
    } else {
        servo_set_enable(id, false);
    }
    return web_send_json(req, 200, servo_list_json());
}

static esp_err_t calibration_put(httpd_req_t *req)
{
    char id[ID_LEN];
    if (!query_id(req, id)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    static const char *const KEYS[] = { "min_us", "max_us", "invert" };
    const char *bad = NULL;
    const cJSON *mn = cJSON_GetObjectItem(body, "min_us");
    const cJSON *mx = cJSON_GetObjectItem(body, "max_us");
    const cJSON *inv = cJSON_GetObjectItem(body, "invert");
    servo_limits_t l;
    servo_get_limits(id, &l);
    esp_err_t sent = ESP_FAIL;
    if (!only_keys(body, KEYS, 3, &bad)) {
        sent = web_send_error(req, 400, "bad_request", "unknown field `%s`", bad);
    } else if (!cJSON_IsNumber(mn) || !cJSON_IsNumber(mx) || (inv != NULL && !cJSON_IsBool(inv)) ||
               mn->valuedouble < l.abs_min_us || mx->valuedouble > l.abs_max_us || mn->valuedouble >= mx->valuedouble) {
        sent = web_send_error(req, 400, "bad_request", "the working range must be within %u..%u, and min below max",
                              l.abs_min_us, l.abs_max_us);
    } else if (!servo_set_op_limits(id, (uint16_t)mn->valuedouble, (uint16_t)mx->valuedouble, cJSON_IsTrue(inv))) {
        sent = web_send_error(req, 500, "failed", "%s: the range was not saved", id);
    } else {
        sent = web_send_json(req, 200, servo_json(id));
    }
    cJSON_Delete(body);
    return sent == ESP_FAIL ? ESP_OK : sent;
}

static esp_err_t calibration_delete(httpd_req_t *req)
{
    char id[ID_LEN];
    if (!query_id(req, id)) {
        return ESP_OK;
    }
    if (!servo_clear_op_limits(id)) {
        return web_send_error(req, 500, "failed", "%s: not cleared", id);
    }
    return web_send_json(req, 200, servo_json(id));
}

static int drive_of(const cJSON *v)
{
    if (!cJSON_IsString(v)) {
        return -1;
    }
    return strcmp(v->valuestring, "hold") == 0 ? SERVO_DRIVE_HOLD
         : strcmp(v->valuestring, "release") == 0 ? SERVO_DRIVE_RELEASE : -1;
}

static esp_err_t policy_put(httpd_req_t *req)
{
    char id[ID_LEN];
    if (!query_id(req, id)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    static const char *const KEYS[] = { "closed", "open", "mid", "settle_ms" };
    const char *bad = NULL;
    const int c = drive_of(cJSON_GetObjectItem(body, "closed"));
    const int o = drive_of(cJSON_GetObjectItem(body, "open"));
    const int m = drive_of(cJSON_GetObjectItem(body, "mid"));
    const cJSON *settle = cJSON_GetObjectItem(body, "settle_ms");
    esp_err_t sent = ESP_FAIL;
    if (!only_keys(body, KEYS, 4, &bad)) {
        sent = web_send_error(req, 400, "bad_request", "unknown field `%s`", bad);
    } else if (c < 0 || o < 0 || m < 0 || !cJSON_IsNumber(settle) || settle->valuedouble < 0 ||
               settle->valuedouble > 65535) {
        sent = web_send_error(req, 400, "bad_request", "`closed`, `open` and `mid` are hold or release; "
                              "`settle_ms` is 0..65535");
    } else {
        const servo_drive_policy_t p = { .drive_closed = (uint8_t)c, .drive_open = (uint8_t)o,
                                         .drive_mid = (uint8_t)m, .settle_ms = (uint16_t)settle->valuedouble };
        sent = servo_set_drive_policy(id, &p) ? web_send_json(req, 200, servo_json(id))
                                              : web_send_error(req, 500, "failed", "%s: the policy was not saved", id);
    }
    cJSON_Delete(body);
    return sent == ESP_FAIL ? ESP_OK : sent;
}

esp_err_t web_servo_register(void (*before_move)(void))
{
    s_before_move = before_move;
    esp_err_t err = ESP_OK;
    err |= web_register("/api/v1/servos", HTTP_GET, servos_get, 0);
    err |= web_register("/api/v1/servos/move", HTTP_POST, move_post, WEB_AUTH);
    err |= web_register("/api/v1/servos/release", HTTP_POST, release_post, WEB_AUTH);
    err |= web_register("/api/v1/servos/calibration", HTTP_PUT, calibration_put, WEB_AUTH);
    err |= web_register("/api/v1/servos/calibration", HTTP_DELETE, calibration_delete, WEB_AUTH);
    err |= web_register("/api/v1/servos/policy", HTTP_PUT, policy_put, WEB_AUTH);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}
