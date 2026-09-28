/*
 * web_servo: the /api/v1/servos routes. See web_servo.h.
 *
 * A servo is named by its ident (`id`): in the query for its saved settings (PUT and DELETE
 * /servos/calibration, PUT /servos/policy), in the body for an action. The routes are api_core's:
 * the web server serves them, and a host link the three it may (list, move, release).
 */
#include <stdio.h>
#include <string.h>
#include "servo.h"
#include "api_core.h"
#include "web_servo.h"

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
static api_reply_t no_such_servo(const char *id)
{
    char names[96] = "";
    char one[ID_LEN];
    for (size_t i = 0; servo_ident(i, one, sizeof(one)); i++) {
        snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", i ? ", " : "", one);
    }
    return api_error(404, "not_found", "no servo '%s' (there is: %s)", id, names[0] ? names : "none");
}

/* The query's `id`: false with the error in `err`. */
static bool query_id(const api_req_t *req, char *id, api_reply_t *err)
{
    if (!api_query(req, "id", id, ID_LEN) || id[0] == '\0') {
        *err = api_error(400, "bad_request", "give `id`, a servo as GET /api/v1/servos lists them");
        return false;
    }
    if (!servo_exists(id)) {
        *err = no_such_servo(id);
        return false;
    }
    return true;
}

/* A body's `id`: false with the error in `err`. `all` allowed if asked. */
static bool body_id(const cJSON *body, char *id, bool all, api_reply_t *err)
{
    const cJSON *v = cJSON_GetObjectItem(body, "id");
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= ID_LEN) {
        *err = api_error(400, "bad_request", "give `id`, a servo as GET /api/v1/servos lists them%s",
                         all ? ", or all" : "");
        return false;
    }
    strlcpy(id, v->valuestring, ID_LEN);
    if ((all && strcmp(id, "all") == 0) || servo_exists(id)) {
        return true;
    }
    *err = no_such_servo(id);
    return false;
}

/* ------------------------------------------------------------------ routes */

static api_reply_t servos_get(const api_req_t *req)
{
    (void)req;
    return api_json(200, servo_list_json());
}

static api_reply_t move_post(const api_req_t *req)
{
    const cJSON *body = req->body;
    char id[ID_LEN];
    static const char *const KEYS[] = { "id", "us", "percent", NULL };
    const char *bad = NULL;
    const cJSON *us = cJSON_GetObjectItem(body, "us");
    const cJSON *pct = cJSON_GetObjectItem(body, "percent");
    api_reply_t err;
    if (!api_only_keys(body, KEYS, &bad)) {
        return api_error(400, "bad_request", "unknown field `%s`", bad);
    }
    if (!body_id(body, id, false, &err)) {
        return err;
    }
    if ((us == NULL) == (pct == NULL) || (us && !cJSON_IsNumber(us)) || (pct && !cJSON_IsNumber(pct))) {
        return api_error(400, "bad_request", "give `us` or `percent`, a number");
    }
    servo_limits_t l;
    servo_get_limits(id, &l);
    if (us != NULL && (us->valuedouble < l.abs_min_us || us->valuedouble > l.abs_max_us)) {
        return api_error(400, "bad_request", "`us` must be within %u..%u", l.abs_min_us, l.abs_max_us);
    }
    if (pct != NULL && (pct->valuedouble < 0 || pct->valuedouble > 100)) {
        return api_error(400, "bad_request", "`percent` is 0..100");
    }
    if (s_before_move != NULL) {
        s_before_move();
    }
    const bool ok = us != NULL ? servo_move_us(id, (uint16_t)us->valuedouble)
                               : servo_set_percentage(id, (uint16_t)(pct->valuedouble * 10 + 0.5));
    servo_set_enable(id, true);
    return ok ? api_json(200, servo_json(id)) : api_error(500, "failed", "%s did not move", id);
}

static api_reply_t release_post(const api_req_t *req)
{
    char id[ID_LEN];
    api_reply_t err;
    if (!body_id(req->body, id, true, &err)) {
        return err;
    }
    if (strcmp(id, "all") == 0) {
        char one[ID_LEN];
        for (size_t i = 0; servo_ident(i, one, sizeof(one)); i++) {
            servo_set_enable(one, false);
        }
    } else {
        servo_set_enable(id, false);
    }
    return api_json(200, servo_list_json());
}

static api_reply_t calibration_put(const api_req_t *req)
{
    char id[ID_LEN];
    api_reply_t err;
    if (!query_id(req, id, &err)) {
        return err;
    }
    const cJSON *body = req->body;
    static const char *const KEYS[] = { "min_us", "max_us", "invert", NULL };
    const char *bad = NULL;
    const cJSON *mn = cJSON_GetObjectItem(body, "min_us");
    const cJSON *mx = cJSON_GetObjectItem(body, "max_us");
    const cJSON *inv = cJSON_GetObjectItem(body, "invert");
    servo_limits_t l;
    servo_get_limits(id, &l);
    if (!api_only_keys(body, KEYS, &bad)) {
        return api_error(400, "bad_request", "unknown field `%s`", bad);
    }
    if (!cJSON_IsNumber(mn) || !cJSON_IsNumber(mx) || (inv != NULL && !cJSON_IsBool(inv)) ||
        mn->valuedouble < l.abs_min_us || mx->valuedouble > l.abs_max_us || mn->valuedouble >= mx->valuedouble) {
        return api_error(400, "bad_request", "the working range must be within %u..%u, and min below max",
                         l.abs_min_us, l.abs_max_us);
    }
    if (!servo_set_op_limits(id, (uint16_t)mn->valuedouble, (uint16_t)mx->valuedouble, cJSON_IsTrue(inv))) {
        return api_error(500, "failed", "%s: the range was not saved", id);
    }
    return api_json(200, servo_json(id));
}

static api_reply_t calibration_delete(const api_req_t *req)
{
    char id[ID_LEN];
    api_reply_t err;
    if (!query_id(req, id, &err)) {
        return err;
    }
    if (!servo_clear_op_limits(id)) {
        return api_error(500, "failed", "%s: not cleared", id);
    }
    return api_json(200, servo_json(id));
}

static int drive_of(const cJSON *v)
{
    if (!cJSON_IsString(v)) {
        return -1;
    }
    return strcmp(v->valuestring, "hold") == 0 ? SERVO_DRIVE_HOLD
         : strcmp(v->valuestring, "release") == 0 ? SERVO_DRIVE_RELEASE : -1;
}

static api_reply_t policy_put(const api_req_t *req)
{
    char id[ID_LEN];
    api_reply_t err;
    if (!query_id(req, id, &err)) {
        return err;
    }
    const cJSON *body = req->body;
    static const char *const KEYS[] = { "closed", "open", "mid", "settle_ms", NULL };
    const char *bad = NULL;
    const int c = drive_of(cJSON_GetObjectItem(body, "closed"));
    const int o = drive_of(cJSON_GetObjectItem(body, "open"));
    const int m = drive_of(cJSON_GetObjectItem(body, "mid"));
    const cJSON *settle = cJSON_GetObjectItem(body, "settle_ms");
    if (!api_only_keys(body, KEYS, &bad)) {
        return api_error(400, "bad_request", "unknown field `%s`", bad);
    }
    if (c < 0 || o < 0 || m < 0 || !cJSON_IsNumber(settle) || settle->valuedouble < 0 || settle->valuedouble > 65535) {
        return api_error(400, "bad_request", "`closed`, `open` and `mid` are hold or release; `settle_ms` is 0..65535");
    }
    const servo_drive_policy_t p = { .drive_closed = (uint8_t)c, .drive_open = (uint8_t)o,
                                     .drive_mid = (uint8_t)m, .settle_ms = (uint16_t)settle->valuedouble };
    return servo_set_drive_policy(id, &p) ? api_json(200, servo_json(id))
                                          : api_error(500, "failed", "%s: the policy was not saved", id);
}

static const api_route_t ROUTES[] = {
    API_ROUTE(API_GET, "/api/v1/servos", servos_get, 0, API_LINK),
    API_ROUTE(API_POST, "/api/v1/servos/move", move_post, 256, API_LINK),
    API_ROUTE(API_POST, "/api/v1/servos/release", release_post, 128, API_LINK),
    API_ROUTE(API_PUT, "/api/v1/servos/calibration", calibration_put, 256, 0),
    API_ROUTE(API_DELETE, "/api/v1/servos/calibration", calibration_delete, 0, 0),
    API_ROUTE(API_PUT, "/api/v1/servos/policy", policy_put, 256, 0),
};

esp_err_t web_servo_register(void (*before_move)(void))
{
    s_before_move = before_move;
    return api_add_routes(ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]));
}
