/*
 * holo: a holoprojector's motions and light, and the `holo` command. See holo.h.
 *
 * Ported from r2_domeplayer's main/panel_holo.c, the dome's basic holoprojectors. The dome's
 * joint registry and arm switch became holo_config_t's hooks; without them the axes go
 * straight through servo.h.
 *
 * A position is x and y from -1 (full left, down) to +1 (full right, up), 0 at the centre
 * of the calibrated travel; the console speaks them in percent. Every motion is a chain of
 * segments, eased in and out except the circle's; the behaviours only choose the next one.
 * A behaviour owns its axes while it runs: move one from elsewhere meanwhile and the next
 * segment starts from where this file last put it. A new command starts from where the axis
 * hook says the axes are, so it never jumps.
 *
 * Arming gates motion and nothing else. A motion command while disarmed is refused, and a
 * disarm stops every behaviour where it is. The light is not motion and works either way.
 *
 * A holo's light, where it has one, is a servo channel driven as plain PWM (servo_set_duty),
 * at the servos' 50 Hz. Brightness goes through gamma 2, so a pulse looks even to the eye.
 *
 * One task, ticking every 20 ms -- the servo frame -- while anything animates, and waking
 * once a second otherwise to refresh the lights: that is what brings a light back after a
 * controller reset, and what claims a light's channel on a board that answered late. Its
 * stack is in PSRAM where there is PSRAM, which is safe only because nothing on this path
 * writes flash.
 */
#include "holo.h"
#include "servo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "holo";

#define TICK_MS          20      /* the servo frame */
#define IDLE_REFRESH_MS  1000
#define TASK_STACK       3072
#define CIRCLE_STEPS     24      /* straight segments a turn */

typedef enum { MOT_HOLD, MOT_MOVE, MOT_TWITCH, MOT_WAG, MOT_NOD, MOT_SCAN, MOT_CIRCLE } motion_t;
static const char *const MOTION_NAMES[] = { "hold", "move", "twitch", "wag", "nod", "scan", "circle" };

typedef enum { LED_OFF, LED_ON, LED_PULSE, LED_FLICKER, LED_BLINK } led_mode_t;
static const char *const LED_NAMES[] = { "off", "on", "pulse", "flicker", "blink" };

typedef struct {
    led_mode_t mode;
    uint8_t level;          /* percent */
    uint32_t period_ms;     /* pulse and blink */
} led_cfg_t;

typedef struct {
    /* The light. With `led_until` set, `led` becomes `after` at that time: a timed mode. */
    led_cfg_t led;
    led_cfg_t after;
    int64_t led_t0;
    int64_t led_until;
    uint16_t duty;          /* last sent, of SERVO_DUTY_FULL */

    /* Motion. x and y are where the axes were last put, -1..1. */
    motion_t motion;
    float x, y;
    bool moving;            /* a segment is in flight */
    float x0, y0, x1, y1;
    int64_t seg_t0, seg_us;
    bool ease;
    int64_t motion_until;   /* twitch and scan with -t: hold from then */
    /* the behaviour's parameters */
    float range;            /* 0..1 of the half-travel */
    uint32_t period_ms;
    int total, left;        /* segments in all, and still to run: wag, nod, circle */
    float ox, oy;           /* where wag, nod and circle return to */
    uint32_t min_ms, max_ms;/* twitch: the pause between glances */
    int64_t next_us;        /* twitch: when the next glance starts */
    int step;               /* scan: which end is next */
    uint16_t sent_h, sent_v;/* permille last sent; 0xffff for "send the next one whatever it is" */
    const char *stopped;    /* why the last motion ended early, NULL if it did not */
} holo_t;

/* With the default hooks, whether this file has moved each axis since boot: before that,
 * the pulse the servo holds is only its parking value. Two per holo, horizontal then
 * vertical. */
typedef struct {
    bool placed;
} spot_t;

static const holo_desc_t *s_desc;
static size_t s_count;
static holo_config_t s_cfg;
static holo_t *s_holo;
static spot_t *s_spot;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

/* ---- the axes: the hooks, or servo.h ----------------------------------------- */

static spot_t *spot_of(const char *ident)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_desc[i].h, ident) == 0) return &s_spot[2 * i];
        if (strcmp(s_desc[i].v, ident) == 0) return &s_spot[2 * i + 1];
    }
    return NULL;
}

static bool def_axis(const char *ident, holo_axis_t *out)
{
    memset(out, 0, sizeof(*out));
    servo_limits_t l;
    if (!servo_get_limits(ident, &l)) {
        return true;   /* a holo's axis, but no such servo (yet): not fitted */
    }
    out->fitted = true;
    out->calibrated = l.calibrated || s_cfg.allow_uncalibrated;
    out->closed_us = l.invert ? l.op_max_us : l.op_min_us;
    out->open_us = l.invert ? l.op_min_us : l.op_max_us;
    const spot_t *s = spot_of(ident);
    uint16_t us = 0;
    out->placed = s != NULL && s->placed && servo_get_value_us(ident, &us);
    if (out->placed) {
        /* From the pulse last sent, against the endpoints as they are now: a calibration
         * saved since then changes where that pulse sits in the travel, not the servo. */
        const int span = (int)out->open_us - (int)out->closed_us;
        const int p = span != 0 ? ((int)us - (int)out->closed_us) * 1000 / span : 500;
        out->permille = (uint16_t)(p < 0 ? 0 : (p > 1000 ? 1000 : p));
        out->us = us;
    } else {
        out->permille = 500;
        out->us = (uint16_t)(((int)out->closed_us + (int)out->open_us) / 2);
    }
    out->display = l.calibrated ? NULL : "default range, not saved";
    return true;
}

static bool def_axis_move(const char *ident, uint16_t permille, const char **why)
{
    /* servo_set_percentage() maps 0..1000 onto the working range, turned round if the
     * servo is inverted -- which is what an endpoint with closed above open saved. */
    if (!servo_exists(ident) || !servo_set_percentage(ident, permille)) {
        *why = HOLO_WHY_NOT_FITTED;
        return false;
    }
    spot_t *s = spot_of(ident);
    if (s != NULL) {
        s->placed = true;
    }
    return true;
}

static bool def_endpoints(const char *ident, bool clear, uint16_t closed_us, uint16_t open_us,
                          const char **why)
{
    if (!servo_exists(ident)) {
        *why = HOLO_WHY_NOT_FITTED;
        return false;
    }
    const bool ok = clear ? servo_clear_op_limits(ident)
                          : servo_set_op_limits(ident, closed_us < open_us ? closed_us : open_us,
                                                closed_us < open_us ? open_us : closed_us,
                                                closed_us > open_us);
    if (!ok) {
        *why = clear ? "not cleared" : "outside the servo's absolute range, or not saved";
    }
    return ok;
}

static bool is_armed(void)
{
    return s_cfg.armed == NULL || s_cfg.armed(s_cfg.ctx);
}

static bool get_axis(const char *ident, holo_axis_t *out)
{
    return s_cfg.axis != NULL ? s_cfg.axis(ident, out, s_cfg.ctx) : def_axis(ident, out);
}

static bool move_axis(const char *ident, uint16_t permille, const char **why)
{
    return s_cfg.axis_move != NULL ? s_cfg.axis_move(ident, permille, why, s_cfg.ctx)
                                   : def_axis_move(ident, permille, why);
}

static bool set_endpoints(const char *ident, bool clear, uint16_t closed_us, uint16_t open_us,
                          const char **why)
{
    return s_cfg.endpoints != NULL ? s_cfg.endpoints(ident, clear, closed_us, open_us, why, s_cfg.ctx)
                                   : def_endpoints(ident, clear, closed_us, open_us, why);
}

static void release_axis(const char *ident)
{
    if (s_cfg.axis_release != NULL) {
        s_cfg.axis_release(ident, s_cfg.ctx);
    } else if (servo_exists(ident)) {
        servo_set_enable(ident, false);
    }
}

/* ---- arithmetic ------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* [0, 1) */
static float frand(void)
{
    return (float)(esp_random() >> 8) / 16777216.0f;
}

static uint16_t to_permille(float p)
{
    return (uint16_t)lroundf((clampf(p, -1.0f, 1.0f) + 1.0f) * 500.0f);
}

static float from_permille(uint16_t p)
{
    return (float)p / 500.0f - 1.0f;
}

/* How long a move takes: `base_ms`, plus `per_ms` for each whole half-travel of the
 * longer axis -- so a hop is quick and a sweep across is a little over a second. */
static int64_t travel_us(float dx, float dy, uint32_t base_ms, uint32_t per_ms)
{
    const float d = fmaxf(fabsf(dx), fabsf(dy));
    return (int64_t)((float)base_ms + (float)per_ms * d) * 1000;
}

/* ---- motion ---------------------------------------------------------------- */

/* Where the axes are, as the axis hook has it: something else may have moved them since
 * this file last did. An axis nothing has moved since boot is taken to be at the centre,
 * which is where the servo component parked its pulse. */
static void locate(size_t i)
{
    holo_t *h = &s_holo[i];
    holo_axis_t a;
    h->x = (get_axis(s_desc[i].h, &a) && a.placed) ? from_permille(a.permille) : 0.0f;
    h->y = (get_axis(s_desc[i].v, &a) && a.placed) ? from_permille(a.permille) : 0.0f;
}

static void segment(holo_t *h, float x1, float y1, int64_t dur_us, bool ease, int64_t now)
{
    h->x0 = h->x;
    h->y0 = h->y;
    h->x1 = clampf(x1, -1.0f, 1.0f);
    h->y1 = clampf(y1, -1.0f, 1.0f);
    h->seg_t0 = now;
    h->seg_us = dur_us > 0 ? dur_us : 1;
    h->ease = ease;
    h->moving = true;
}

static void stop_motion(size_t i, const char *why)
{
    holo_t *h = &s_holo[i];
    if (why != NULL && (h->moving || h->motion != MOT_HOLD)) {
        ESP_LOGI(TAG, "%s: %s stopped: %s", s_desc[i].name, MOTION_NAMES[h->motion], why);
    }
    h->motion = MOT_HOLD;
    h->moving = false;
    h->motion_until = 0;
    h->stopped = why;
}

/* The segment after the one that just ended, or the end of the behaviour. */
static void next_segment(size_t i, int64_t now)
{
    holo_t *h = &s_holo[i];
    switch (h->motion) {
    case MOT_HOLD:
    case MOT_MOVE:
        h->motion = MOT_HOLD;
        break;
    case MOT_TWITCH:
        /* A glance, then a pause of its own length before the next. */
        h->next_us = now + (int64_t)((float)h->min_ms + frand() * (float)(h->max_ms - h->min_ms)) * 1000;
        break;
    case MOT_WAG:
    case MOT_NOD: {
        /* +r, -r, +r ... about where it started, then back there: a quarter period out, a
         * half period for each swing across, a quarter period home. */
        if (h->left <= 0) {
            h->motion = MOT_HOLD;
            break;
        }
        const int k = h->total - h->left--;
        float off = 0.0f;
        uint32_t ms = h->period_ms / 4;
        if (h->left > 0) {
            off = (k & 1) ? -h->range : h->range;
            if (k > 0) ms = h->period_ms / 2;
        }
        if (h->motion == MOT_WAG) {
            segment(h, h->ox + off, h->y, (int64_t)ms * 1000, true, now);
        } else {
            segment(h, h->x, h->oy + off, (int64_t)ms * 1000, true, now);
        }
        break;
    }
    case MOT_SCAN: {
        /* End to end across the middle, each leg half a period; the first is scaled to
         * however far it has to go. */
        const float target = (h->step++ & 1) ? -h->range : h->range;
        const float frac = fminf(fabsf(target - h->x) / (2.0f * h->range), 1.0f);
        segment(h, target, h->y, (int64_t)((float)h->period_ms / 2.0f * frac) * 1000 + 1000, true, now);
        break;
    }
    case MOT_CIRCLE: {
        /* Out to the rim, CIRCLE_STEPS straight legs a turn at an even speed, and home. */
        if (h->left <= 0) {
            h->motion = MOT_HOLD;
            break;
        }
        const int k = h->total - h->left--;
        if (k == 0) {
            segment(h, h->ox + h->range, h->oy, travel_us(h->range, 0, 150, 500), true, now);
        } else if (h->left == 0) {
            segment(h, h->ox, h->oy, travel_us(h->range, 0, 150, 500), true, now);
        } else {
            const float a = 2.0f * (float)M_PI * (float)k / CIRCLE_STEPS;
            segment(h, h->ox + h->range * cosf(a), h->oy + h->range * sinf(a),
                    (int64_t)h->period_ms * 1000 / CIRCLE_STEPS, false, now);
        }
        break;
    }
    }
}

/* Put the axes where x and y say. Only an axis whose permille changed is sent. */
static bool send(size_t i, const char **why)
{
    holo_t *h = &s_holo[i];
    const uint16_t ph = to_permille(h->x), pv = to_permille(h->y);
    if (ph != h->sent_h) {
        if (!move_axis(s_desc[i].h, ph, why)) return false;
        h->sent_h = ph;
    }
    if (pv != h->sent_v) {
        if (!move_axis(s_desc[i].v, pv, why)) return false;
        h->sent_v = pv;
    }
    return true;
}

/* ---- the light --------------------------------------------------------------- */

static bool led_animated(const holo_t *h)
{
    return h->led.mode == LED_PULSE || h->led.mode == LED_FLICKER || h->led.mode == LED_BLINK ||
           h->led_until != 0;
}

static uint16_t led_duty(const holo_t *h, int64_t now)
{
    const led_cfg_t *c = &h->led;
    const float full = (float)c->level / 100.0f;
    const uint32_t ms = (uint32_t)((now - h->led_t0) / 1000);
    float b = 0.0f;
    switch (c->mode) {
    case LED_OFF:
        break;
    case LED_ON:
        b = full;
        break;
    case LED_PULSE:
        b = full * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)(ms % c->period_ms) / (float)c->period_ms));
        break;
    case LED_BLINK:
        b = (ms % c->period_ms) < c->period_ms / 2 ? full : 0.0f;
        break;
    case LED_FLICKER:
        /* A failing projector: restless and mostly bright, with the odd dropout. */
        b = full * (frand() < 0.05f ? 0.1f + 0.2f * frand() : 0.55f + 0.45f * frand());
        break;
    }
    return (uint16_t)lroundf(b * b * (float)SERVO_DUTY_FULL);
}

/* ---- the engine -------------------------------------------------------------- */

/* One frame of one holo. True while it animates, so the task keeps its 20 ms tick. */
static bool tick_holo(size_t i, int64_t now, bool armed)
{
    holo_t *h = &s_holo[i];

    if (s_desc[i].led != NULL) {
        if (h->led_until != 0 && now >= h->led_until) {
            h->led = h->after;
            h->led_t0 = now;
            h->led_until = 0;
        }
        h->duty = led_duty(h, now);
        servo_set_duty(s_desc[i].led, h->duty);   /* false until its board answers: nothing to do */
    }

    if (h->motion == MOT_HOLD && !h->moving) {
        return led_animated(h);
    }
    if (!armed) {
        stop_motion(i, HOLO_WHY_NOT_ARMED);
        return led_animated(h);
    }
    if (h->motion_until != 0 && now >= h->motion_until && !h->moving) {
        stop_motion(i, NULL);
        return led_animated(h);
    }
    if (h->moving) {
        float f = (float)(now - h->seg_t0) / (float)h->seg_us;
        if (f > 1.0f) f = 1.0f;
        const float e = h->ease ? f * f * (3.0f - 2.0f * f) : f;
        h->x = h->x0 + (h->x1 - h->x0) * e;
        h->y = h->y0 + (h->y1 - h->y0) * e;
        const char *why = NULL;
        if (!send(i, &why)) {
            stop_motion(i, why);
            return led_animated(h);
        }
        if (f >= 1.0f) {
            h->moving = false;
            next_segment(i, now);
        }
    } else if (h->motion == MOT_TWITCH && now >= h->next_us) {
        /* Anywhere in a disc of the range about the centre, evenly over its area. */
        const float a = 2.0f * (float)M_PI * frand();
        const float r = h->range * sqrtf(frand());
        const float tx = r * cosf(a), ty = r * sinf(a);
        segment(h, tx, ty, travel_us(tx - h->x, ty - h->y, 80, 150), true, now);
    }
    return true;
}

static void holo_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        const bool armed = is_armed();
        bool busy = false;
        for (size_t i = 0; i < s_count; i++) {
            busy |= tick_holo(i, now, armed);
        }
        xSemaphoreGive(s_lock);
        if (busy) {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_REFRESH_MS));
            last = xTaskGetTickCount();
        }
    }
}

esp_err_t holo_start(const holo_desc_t *holos, size_t count, const holo_config_t *config)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    if (holos == NULL || count == 0) return ESP_ERR_INVALID_ARG;
    s_desc = holos;
    s_count = count;
    if (config != NULL) s_cfg = *config;

    s_lock = xSemaphoreCreateMutex();
    /* PSRAM where there is some: internal DRAM is the tight pool. */
    s_holo = heap_caps_calloc_prefer(count, sizeof(holo_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_8BIT);
    s_spot = calloc(2 * count, sizeof(spot_t));
    if (s_lock == NULL || s_holo == NULL || s_spot == NULL) {
        ESP_LOGE(TAG, "out of memory; the holoprojectors are off");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        s_holo[i] = (holo_t){ .led = { LED_OFF, 100, 2000 }, .sent_h = 0xffff, .sent_v = 0xffff };
    }
    /* A PSRAM stack where there is PSRAM: safe only because nothing on this path writes
     * flash. Internal RAM otherwise. */
    if (xTaskCreateWithCaps(holo_task, "holo", TASK_STACK, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreate(holo_task, "holo", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no task; the holoprojectors are off");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- the console ------------------------------------------------------------- */

static const char *USAGE =
    "usage: holo                              status: each holo's light, motion and axes\n"
    "       holo <holo> center [-d ms]\n"
    "       holo <holo> move <x> <y> [-d ms]  x, y -100..100: percent of the way from the centre\n"
    "                                         to an endpoint; + is right and up\n"
    "       holo <holo> nudge <dx> <dy> [-d ms]\n"
    "       holo <holo> twitch [-r pct] [-i s-s] [-t s]   random glances, until stopped\n"
    "       holo <holo> wag [-n cycles] [-r pct] [-p ms]  side to side, and back\n"
    "       holo <holo> nod [-n cycles] [-r pct] [-p ms]  up and down, and back\n"
    "       holo <holo> scan [-r pct] [-p ms] [-t s]      a slow sweep, until stopped\n"
    "       holo <holo> circle [-n turns] [-r pct] [-p ms]\n"
    "       holo <holo> stop                  motion stops where it is\n"
    "       holo <holo> led off | on [pct] | pulse [-l pct] [-p ms] | flicker [-l pct]\n"
    "                       | blink [-l pct] [-p ms]    ... [-t s]: then back to what it was\n"
    "       holo <holo> leia [-t s]           centre, and flicker, then the light goes back\n"
    "       holo <holo> off                   stop, the axes limp, and the light off\n"
    "       holo <holo> endpoints <h|v> <closed> <open> | clear\n"
    "                                         an axis's travel in us, saved; closed above open\n"
    "                                         turns the axis round\n"
    "Motion needs both axes' endpoints set: CLOSED at full left or down, OPEN at full right\n"
    "or up. The light needs neither.\n";

static void print_usage(void)
{
    printf("%s", USAGE);
    printf("<holo> is ");
    for (size_t i = 0; i < s_count; i++) {
        printf("%s, ", s_desc[i].name);
    }
    printf("or all%s.\n", s_count == 1 ? "; with one holo it may be left out" : "");
    if (s_cfg.armed != NULL) {
        printf("Motion also needs arming.\n");
    }
}

typedef struct {
    long d, r, n, p, l;        /* -d ms, -r pct, -n count, -p ms, -l pct */
    float t;                   /* -t s */
    float imin, imax;          /* -i s-s */
    bool has_d, has_r, has_n, has_p, has_l, has_t, has_i;
} opts_t;

static bool parse_long(const char *s, long lo, long hi, long *out)
{
    char *end;
    const long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool parse_float(const char *s, float lo, float hi, float *out)
{
    char *end;
    const float v = strtof(s, &end);
    if (*s == '\0' || *end != '\0' || !(v >= lo && v <= hi)) return false;
    *out = v;
    return true;
}

/* The options from argv[from] on; `allowed` lists the letters this verb takes. Positional
 * arguments are not options, so a verb reads those before calling this. */
static bool parse_opts(int argc, char **argv, int from, const char *allowed, opts_t *o)
{
    for (int i = from; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0' || a[2] != '\0' || strchr(allowed, a[1]) == NULL || i + 1 >= argc) {
            printf("holo: '%s' is not an option here (this takes %s%s)\n", a,
                   *allowed ? "-" : "none", allowed);
            return false;
        }
        const char *v = argv[++i];
        bool ok = false;
        switch (a[1]) {
        case 'd': ok = o->has_d = parse_long(v, 0, 10000, &o->d); break;
        case 'r': ok = o->has_r = parse_long(v, 1, 100, &o->r); break;
        case 'n': ok = o->has_n = parse_long(v, 1, 20, &o->n); break;
        case 'p': ok = o->has_p = parse_long(v, 200, 60000, &o->p); break;
        case 'l': ok = o->has_l = parse_long(v, 0, 100, &o->l); break;
        case 't': ok = o->has_t = parse_float(v, 0.1f, 3600.0f, &o->t); break;
        case 'i': {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s", v);
            char *dash = strchr(buf, '-');
            if (dash != NULL) *dash = '\0';
            ok = parse_float(buf, 0.1f, 600.0f, &o->imin) &&
                 parse_float(dash ? dash + 1 : buf, 0.1f, 600.0f, &o->imax) && o->imin <= o->imax;
            o->has_i = ok;
            break;
        }
        }
        if (!ok) {
            printf("holo: %s %s is out of range\n", a, v);
            return false;
        }
    }
    return true;
}

/* Why this holo cannot move now, or NULL. Checked when the command is given, so the console
 * hears a refusal at once rather than finding the motion stopped later; the engine checks
 * again every frame. `ident` names the axis at fault. */
static const char *motion_refusal(size_t i, const char **ident)
{
    const char *axes[2] = { s_desc[i].h, s_desc[i].v };
    holo_axis_t a[2];
    for (int k = 0; k < 2; k++) {
        *ident = axes[k];
        if (!get_axis(axes[k], &a[k])) return HOLO_WHY_UNKNOWN;
        if (!a[k].fitted) return HOLO_WHY_NOT_FITTED;
    }
    *ident = NULL;
    if (!is_armed()) return HOLO_WHY_NOT_ARMED;
    for (int k = 0; k < 2; k++) {
        *ident = axes[k];
        if (!a[k].calibrated) return HOLO_WHY_UNCALIBRATED;
    }
    *ident = NULL;
    return NULL;
}

static bool refuse_motion(size_t i)
{
    const char *ident = NULL;
    const char *why = motion_refusal(i, &ident);
    if (why == NULL) return false;
    if (ident != NULL) {
        printf("%s: refused, %s (%s)\n", s_desc[i].name, why, ident);
    } else {
        printf("%s: refused, %s\n", s_desc[i].name, why);
    }
    return true;
}

/* A new motion: from where the axis hook says the axes are, sending the first frame
 * whatever it is, so the channels are driven from the start. */
static void begin(size_t i, motion_t m, const opts_t *o, int64_t now)
{
    holo_t *h = &s_holo[i];
    locate(i);
    h->motion = m;
    h->moving = false;
    h->stopped = NULL;
    h->sent_h = h->sent_v = 0xffff;
    h->ox = h->x;
    h->oy = h->y;
    h->step = 0;
    h->motion_until = (o != NULL && o->has_t) ? now + (int64_t)(o->t * 1e6f) : 0;
}

static int cmd_motion(size_t i, const char *verb, int argc, char **argv)
{
    holo_t *h = &s_holo[i];
    opts_t o = { 0 };
    const int64_t now = esp_timer_get_time();

    /* Known verbs first: an unknown one is a usage error, not a motion to refuse. */
    static const char *const VERBS[] = { "stop", "center", "centre", "move", "nudge", "twitch",
                                         "wag", "nod", "scan", "circle" };
    bool known = false;
    for (size_t k = 0; k < sizeof(VERBS) / sizeof(VERBS[0]); k++) {
        known |= strcmp(verb, VERBS[k]) == 0;
    }
    if (!known) return -1;

    if (strcmp(verb, "stop") == 0) {
        if (argc > 3) return printf("holo: stop takes nothing\n"), 1;
        stop_motion(i, NULL);
        printf("%s: holding\n", s_desc[i].name);
        return 0;
    }

    long x = 0, y = 0;
    const bool is_move = strcmp(verb, "move") == 0, is_nudge = strcmp(verb, "nudge") == 0;
    const bool is_center = strcmp(verb, "center") == 0 || strcmp(verb, "centre") == 0;
    int from = 3;
    if (is_move || is_nudge) {
        const long lim = is_move ? 100 : 200;
        if (argc < 5 || !parse_long(argv[3], -lim, lim, &x) || !parse_long(argv[4], -lim, lim, &y)) {
            printf("holo: %s takes <x> <y>, each -%ld..%ld\n", verb, lim, lim);
            return 1;
        }
        from = 5;
    }
    const char *allowed = is_move || is_nudge || is_center ? "d"
                        : strcmp(verb, "twitch") == 0 ? "rit"
                        : strcmp(verb, "scan") == 0 ? "rpt"
                        : "nrp";   /* wag, nod, circle */
    if (!parse_opts(argc, argv, from, allowed, &o)) return 1;
    if (refuse_motion(i)) return 1;

    if (is_move || is_nudge || is_center) {
        begin(i, MOT_MOVE, NULL, now);
        float tx = is_center ? 0.0f : (float)x / 100.0f, ty = is_center ? 0.0f : (float)y / 100.0f;
        if (is_nudge) {
            tx += h->x;
            ty += h->y;
        }
        tx = clampf(tx, -1.0f, 1.0f);
        ty = clampf(ty, -1.0f, 1.0f);
        const int64_t dur = o.has_d ? (int64_t)o.d * 1000 : travel_us(tx - h->x, ty - h->y, 250, 500);
        segment(h, tx, ty, dur, true, now);
        printf("%s: to x %+d%% y %+d%% in %d ms\n", s_desc[i].name, (int)lroundf(tx * 100),
               (int)lroundf(ty * 100), (int)(dur / 1000));
        return 0;
    }
    if (strcmp(verb, "twitch") == 0) {
        begin(i, MOT_TWITCH, &o, now);
        h->range = (float)(o.has_r ? o.r : 60) / 100.0f;
        h->min_ms = (uint32_t)((o.has_i ? o.imin : 1.0f) * 1000);
        h->max_ms = (uint32_t)((o.has_i ? o.imax : 5.0f) * 1000);
        h->next_us = now;   /* the first glance at once */
        printf("%s: twitching within %d%% of the centre, every %.1f-%.1f s\n", s_desc[i].name,
               (int)lroundf(h->range * 100), h->min_ms / 1000.0, h->max_ms / 1000.0);
        return 0;
    }
    if (strcmp(verb, "scan") == 0) {
        begin(i, MOT_SCAN, &o, now);
        h->range = (float)(o.has_r ? o.r : 70) / 100.0f;
        h->period_ms = (uint32_t)(o.has_p ? o.p : 6000);
        next_segment(i, now);
        printf("%s: scanning %d%% each side, %u ms a sweep there and back\n", s_desc[i].name,
               (int)lroundf(h->range * 100), (unsigned)h->period_ms);
        return 0;
    }
    const bool wag = strcmp(verb, "wag") == 0, nod = strcmp(verb, "nod") == 0;
    if (wag || nod || strcmp(verb, "circle") == 0) {
        begin(i, wag ? MOT_WAG : nod ? MOT_NOD : MOT_CIRCLE, NULL, now);
        const long n = o.has_n ? o.n : (wag ? 3 : nod ? 2 : 1);
        h->range = (float)(o.has_r ? o.r : (wag ? 50 : nod ? 40 : 50)) / 100.0f;
        h->period_ms = (uint32_t)(o.has_p ? o.p : (wag ? 700 : nod ? 800 : 2400));
        h->total = h->left = wag || nod ? (int)(2 * n + 1) : (int)(CIRCLE_STEPS * n + 2);
        next_segment(i, now);
        printf("%s: %s %ld %s of %d%%, %u ms each\n", s_desc[i].name, verb, n,
               wag || nod ? "cycles" : "turns", (int)lroundf(h->range * 100), (unsigned)h->period_ms);
        return 0;
    }
    return -1;   /* not a motion verb */
}

static void set_led(size_t i, led_cfg_t cfg, const opts_t *o, int64_t now)
{
    holo_t *h = &s_holo[i];
    if (o != NULL && o->has_t) {
        /* Timed: back to what it was, unless it was already timed -- then back to what
         * that one would have gone back to, so a chain of timed modes unwinds in one. */
        if (h->led_until == 0) h->after = h->led;
        h->led_until = now + (int64_t)(o->t * 1e6f);
    } else {
        h->led_until = 0;
    }
    h->led = cfg;
    h->led_t0 = now;
}

/* A holo with no light, or one whose channel is not there: say so, and refuse. */
static bool refuse_light(size_t i)
{
    if (s_desc[i].led == NULL) {
        printf("%s: refused, it has no light\n", s_desc[i].name);
        return true;
    }
    if (!servo_exists(s_desc[i].led)) {
        printf("%s: refused, %s (%s)\n", s_desc[i].name, HOLO_WHY_NOT_FITTED, s_desc[i].led);
        return true;
    }
    return false;
}

static int cmd_led(size_t i, int argc, char **argv)
{
    opts_t o = { 0 };
    const int64_t now = esp_timer_get_time();
    if (argc < 4) return printf("holo: led takes off|on [pct]|pulse|flicker|blink\n"), 1;
    if (refuse_light(i)) return 1;
    const char *m = argv[3];
    led_cfg_t cfg = { LED_ON, 100, 0 };
    long level;
    if (parse_long(m, 0, 100, &level)) {                  /* `led 40`: on at 40% */
        cfg.level = (uint8_t)level;
        if (!parse_opts(argc, argv, 4, "t", &o)) return 1;
    } else if (strcmp(m, "off") == 0) {
        cfg.mode = LED_OFF;
        if (!parse_opts(argc, argv, 4, "t", &o)) return 1;
    } else if (strcmp(m, "on") == 0) {
        int from = 4;
        if (argc > 4 && argv[4][0] != '-') {
            if (!parse_long(argv[4], 0, 100, &level)) return printf("holo: on takes 0..100\n"), 1;
            cfg.level = (uint8_t)level;
            from = 5;
        }
        if (!parse_opts(argc, argv, from, "t", &o)) return 1;
    } else if (strcmp(m, "pulse") == 0 || strcmp(m, "blink") == 0 || strcmp(m, "flicker") == 0) {
        const bool flicker = m[0] == 'f';
        cfg.mode = flicker ? LED_FLICKER : m[0] == 'p' ? LED_PULSE : LED_BLINK;
        if (!parse_opts(argc, argv, 4, flicker ? "lt" : "lpt", &o)) return 1;
        cfg.level = (uint8_t)(o.has_l ? o.l : 100);
        cfg.period_ms = (uint32_t)(o.has_p ? o.p : (cfg.mode == LED_PULSE ? 2000 : 1000));
    } else {
        printf("holo: led takes off|on [pct]|pulse|flicker|blink\n");
        return 1;
    }
    set_led(i, cfg, &o, now);
    printf("%s: led %s", s_desc[i].name, LED_NAMES[cfg.mode]);
    if (cfg.mode != LED_OFF) printf(" %u%%", cfg.level);
    if (cfg.period_ms) printf(", %u ms", (unsigned)cfg.period_ms);
    if (o.has_t) printf(", for %.1f s", o.t);
    printf("\n");
    return 0;
}

/* An axis's calibration from the console, saved through the endpoints hook (by default,
 * as the servo's working range in NVS). CLOSED is full left (down), OPEN full right (up).
 * Not gated on arming: it moves nothing. The console's stack is internal, so the save runs
 * inline. */
static int cmd_endpoints(size_t i, int argc, char **argv)
{
    long closed = 0, open = 0;
    const bool clear = argc == 5 && strcmp(argv[4], "clear") == 0;
    if (argc < 5 || (strcmp(argv[3], "h") != 0 && strcmp(argv[3], "v") != 0) ||
        (!clear && (argc != 6 || !parse_long(argv[4], 0, 65535, &closed) ||
                    !parse_long(argv[5], 0, 65535, &open)))) {
        printf("holo: endpoints takes <h|v> <closed us> <open us>, or <h|v> clear\n");
        return 1;
    }
    const char *ident = argv[3][0] == 'h' ? s_desc[i].h : s_desc[i].v;
    const char *why = HOLO_WHY_UNKNOWN;
    if (!set_endpoints(ident, clear, (uint16_t)closed, (uint16_t)open, &why)) {
        printf("%s: refused, %s (%s)\n", s_desc[i].name, why, ident);
        return 1;
    }
    if (clear) {
        printf("%s: %s back to its default range\n", s_desc[i].name, ident);
    } else {
        printf("%s: %s closed %ld open %ld us%s\n", s_desc[i].name, ident, closed, open,
               closed > open ? ", turned round" : "");
    }
    return 0;
}

static void print_axis(const char *label, const char *ident)
{
    holo_axis_t a;
    if (!get_axis(ident, &a)) {
        printf("       %-6s %-7s unknown\n", label, ident);
        return;
    }
    if (!a.fitted) {
        printf("       %-6s %-7s not fitted\n", label, ident);
        return;
    }
    if (!a.calibrated) {
        printf("       %-6s %-7s UNSET%s%s%s\n", label, ident, a.display ? " (" : "",
               a.display ? a.display : "", a.display ? ")" : "");
        return;
    }
    printf("       %-6s %-7s closed %u open %u us", label, ident, a.closed_us, a.open_us);
    if (a.placed) {
        printf(", at %u us", a.us);
        bool driven = false, released = false;
        if (servo_get_output(ident, &driven, &released) && !driven) {
            printf(", limp");
        }
    } else {
        printf(", not moved since boot");
    }
    if (a.display != NULL) printf(" (%s)", a.display);
    printf("\n");
}

static void print_holo(size_t i, int64_t now)
{
    const holo_desc_t *d = &s_desc[i];
    const holo_t *h = &s_holo[i];
    printf("%-6s horizontal %s, vertical %s, light %s\n", d->name, d->h, d->v,
           d->led != NULL ? d->led : "none");

    holo_axis_t ax, ay;
    const bool have = get_axis(d->h, &ax) && get_axis(d->v, &ay) && ax.placed && ay.placed;
    printf("       motion %s", MOTION_NAMES[h->motion]);
    if (have) {
        printf(", at x %+d%% y %+d%%", (int)lroundf(from_permille(ax.permille) * 100),
               (int)lroundf(from_permille(ay.permille) * 100));
    } else {
        printf(", position unknown");
    }
    if (h->stopped) printf("; stopped: %s", h->stopped);
    printf("\n");

    if (d->led != NULL) {
        if (!servo_exists(d->led)) {
            printf("       led    not fitted\n");
        } else {
            printf("       led    %s %u%%, duty %u/%u", LED_NAMES[h->led.mode], h->led.level,
                   (unsigned)h->duty, (unsigned)SERVO_DUTY_FULL);
            if (h->led_until != 0) {
                printf(", %.1f s then %s %u%%", (double)(h->led_until - now) / 1e6,
                       LED_NAMES[h->after.mode], h->after.level);
            }
            printf("\n");
        }
    }
    print_axis("h", d->h);
    print_axis("v", d->v);
}

static bool names_holos(const char *word)
{
    if (strcmp(word, "status") == 0 || strcmp(word, "help") == 0 || strcmp(word, "all") == 0) {
        return true;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(word, s_desc[i].name) == 0) return true;
    }
    return false;
}

static int cmd_holo(int argc, char **argv)
{
    if (s_task == NULL) {
        printf("holo: not started\n");
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        print_usage();
        return 0;
    }

    /* With one holo, its name may be left out: `holo wag` is `holo <name> wag`. */
    char *named[argc + 1];
    if (s_count == 1 && argc >= 2 && !names_holos(argv[1])) {
        named[0] = argv[0];
        named[1] = (char *)s_desc[0].name;
        for (int k = 1; k < argc; k++) named[k + 1] = argv[k];
        argv = named;
        argc++;
    }

    /* Which holos: one name, or all. */
    bool which[s_count];
    memset(which, 0, sizeof(which));
    if (argc >= 2 && strcmp(argv[1], "status") != 0) {
        bool found = strcmp(argv[1], "all") == 0;
        for (size_t i = 0; i < s_count; i++) {
            which[i] = found || strcmp(argv[1], s_desc[i].name) == 0;
        }
        found = false;
        for (size_t i = 0; i < s_count; i++) found |= which[i];
        if (!found) {
            print_usage();
            return 1;
        }
    } else {
        for (size_t i = 0; i < s_count; i++) which[i] = true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = 0;
    if (argc < 3) {
        const int64_t now = esp_timer_get_time();
        if (s_cfg.armed != NULL) {
            printf("armed %s\n", is_armed() ? "yes" : "no");
        }
        for (size_t i = 0; i < s_count; i++) {
            if (which[i]) print_holo(i, now);
        }
    } else {
        const char *verb = argv[2];
        for (size_t i = 0; i < s_count; i++) {
            if (!which[i]) continue;
            int r;
            if (strcmp(verb, "led") == 0) {
                r = cmd_led(i, argc, argv);
            } else if (strcmp(verb, "endpoints") == 0) {
                r = cmd_endpoints(i, argc, argv);
            } else if (strcmp(verb, "off") == 0) {
                opts_t o = { 0 };
                if (!parse_opts(argc, argv, 3, "", &o)) {
                    r = 1;
                } else {
                    /* Motion stopped and the axes limp -- the next motion drives them again,
                     * from where they were sent -- and the light off. */
                    stop_motion(i, NULL);
                    release_axis(s_desc[i].h);
                    release_axis(s_desc[i].v);
                    if (s_desc[i].led != NULL) {
                        set_led(i, (led_cfg_t){ LED_OFF, 100, 0 }, NULL, esp_timer_get_time());
                    }
                    printf("%s: off, axes limp\n", s_desc[i].name);
                    r = 0;
                }
            } else if (strcmp(verb, "leia") == 0) {
                opts_t o = { 0 };
                if (!parse_opts(argc, argv, 3, "t", &o) || refuse_light(i)) {
                    r = 1;
                } else {
                    /* The message from Leia: the light flickers for as long as it plays, then
                     * goes back to what it was, and the holo looks straight out if it may. */
                    if (!o.has_t) {
                        o.has_t = true;
                        o.t = 30.0f;
                    }
                    const int64_t now = esp_timer_get_time();
                    set_led(i, (led_cfg_t){ LED_FLICKER, 100, 0 }, &o, now);
                    const char *ident = NULL;
                    const char *why = motion_refusal(i, &ident);
                    if (why == NULL) {
                        begin(i, MOT_MOVE, NULL, now);
                        segment(&s_holo[i], 0, 0, travel_us(s_holo[i].x, s_holo[i].y, 250, 500), true, now);
                        printf("%s: leia for %.1f s, centred\n", s_desc[i].name, o.t);
                    } else {
                        printf("%s: leia for %.1f s, not centred: %s\n", s_desc[i].name, o.t, why);
                    }
                    r = 0;
                }
            } else {
                r = cmd_motion(i, verb, argc, argv);
                if (r < 0) {
                    print_usage();
                    r = 1;
                    rc = 1;
                    break;
                }
            }
            if (r != 0) rc = 1;
        }
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return rc;
}

void holo_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "holo",
        .help = "Holoprojectors: status; center, move, nudge, twitch, wag, nod, scan, circle, stop; "
                "led off|on|pulse|flicker|blink; leia; off; endpoints. `holo help` for the "
                "arguments. Motion needs both axes' endpoints set.",
        .hint = "[status|help|<holo>|all] [center|move|nudge|twitch|wag|nod|scan|circle|stop|led|leia|off|endpoints]",
        .func = &cmd_holo,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
