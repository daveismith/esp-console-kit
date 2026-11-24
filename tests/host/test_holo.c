/*
 * The holo engine's timing on the computer: holo/holo.c built whole into this file (its frame,
 * tick_holo(), is static), against stub headers (stubs/) for FreeRTOS, esp_timer, esp_random,
 * esp_log and esp_console, with servo.h's functions stubbed below and the axes reached through
 * holo_config_t's hooks. The engine's task is never run: the test moves the clock and calls the
 * frame itself, every 20 ms, as the task would.
 *
 * What it checks is when a motion ends: above all that a timed scan ends at its time limit (the
 * engine looked at the limit only while no segment was in flight, and a scan starts its next
 * segment the moment one ends, so it never did), and that the axes hold from then on.
 */
#include "../../holo/holo.c"
#include "check.h"

/* ---- the stubs: the clock, the random numbers, servo.h */

static int64_t s_now_us;
static uint32_t s_rand = 12345;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

uint32_t esp_random(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return s_rand;
}

bool servo_exists(const char *id) { (void)id; return false; }
bool servo_set_enable(const char *id, bool enable) { (void)id; (void)enable; return false; }
bool servo_set_percentage(const char *id, uint16_t p) { (void)id; (void)p; return false; }
bool servo_get_value_us(const char *id, uint16_t *us) { (void)id; (void)us; return false; }
bool servo_get_limits(const char *id, servo_limits_t *out) { (void)id; (void)out; return false; }
bool servo_set_op_limits(const char *id, uint16_t lo, uint16_t hi, bool inv) { (void)id; (void)lo; (void)hi; (void)inv; return false; }
bool servo_clear_op_limits(const char *id) { (void)id; return false; }
bool servo_set_duty(const char *id, uint16_t duty) { (void)id; (void)duty; return false; }
bool servo_get_output(const char *id, bool *driven, bool *released) { (void)id; (void)driven; (void)released; return false; }

/* ---- the axes, through the hooks: two, fitted and calibrated, each move counted */

typedef struct {
    bool placed;
    uint16_t permille;
    int moves;
} fake_axis_t;
static fake_axis_t s_axes[2];   /* "h", "v" */

static fake_axis_t *axis_of(const char *ident)
{
    return &s_axes[strcmp(ident, "h") == 0 ? 0 : 1];
}

static bool hook_axis(const char *ident, holo_axis_t *out, void *ctx)
{
    (void)ctx;
    const fake_axis_t *a = axis_of(ident);
    memset(out, 0, sizeof(*out));
    out->fitted = true;
    out->calibrated = true;
    out->placed = a->placed;
    out->permille = a->placed ? a->permille : 500;
    out->closed_us = 1000;
    out->open_us = 2000;
    return true;
}

static bool hook_move(const char *ident, uint16_t permille, const char **why, void *ctx)
{
    (void)why;
    (void)ctx;
    fake_axis_t *a = axis_of(ident);
    a->placed = true;
    a->permille = permille;
    a->moves++;
    return true;
}

static int moves(void)
{
    return s_axes[0].moves + s_axes[1].moves;
}

/* One 20 ms frame, as the task runs it */
static void frame(void)
{
    s_now_us += TICK_MS * 1000;
    tick_holo(0, s_now_us, true);
}

static const char *motion_now(void)
{
    holo_status_t st;
    return holo_status(0, &st) == ESP_OK ? st.motion : "?";
}

/* Frames until the motion is "hold" or `max_ms` has gone: the milliseconds it took, -1 if it never did */
static int64_t frames_until_hold(int64_t max_ms)
{
    const int64_t t0 = s_now_us;
    while (s_now_us - t0 <= max_ms * 1000) {
        if (strcmp(motion_now(), "hold") == 0) {
            return (s_now_us - t0) / 1000;
        }
        frame();
    }
    return -1;
}

static void start(const holo_motion_t *m)
{
    char why[80];
    CHECK(holo_motion(0, m, why, sizeof(why)) == ESP_OK);
}

/* ---- the cases */

/* A timed scan ends at its limit, holds there, and says it ended as asked */
static void test_timed_scan_ends(void)
{
    const holo_motion_t scan = { .kind = HOLO_SCAN, .range = 40, .period_ms = 2000, .time_s = 2.5f, .duration_ms = -1 };
    start(&scan);
    CHECK(strcmp(motion_now(), "scan") == 0);
    holo_status_t st;
    CHECK(holo_status(0, &st) == ESP_OK && st.motion_remaining_ms == 2500);
    const int64_t took = frames_until_hold(10000);
    CHECK(took >= 2500 && took <= 2500 + TICK_MS);
    CHECK(holo_status(0, &st) == ESP_OK && st.stopped == NULL && st.motion_remaining_ms == UINT32_MAX);
    /* It holds: nothing more is sent to the axes */
    const int before = moves();
    for (int i = 0; i < 200; i++) {
        frame();
    }
    CHECK(moves() == before);
    CHECK(strcmp(motion_now(), "hold") == 0);
}

/* A limit that falls in the middle of a leg ends it there: the scan doesn't run on to the leg's end */
static void test_timed_scan_mid_leg(void)
{
    const holo_motion_t scan = { .kind = HOLO_SCAN, .range = 70, .period_ms = 60000, .time_s = 1.0f, .duration_ms = -1 };
    start(&scan);
    const int64_t took = frames_until_hold(40000);
    CHECK(took >= 1000 && took <= 1000 + TICK_MS);
}

/* A scan with no limit goes on */
static void test_untimed_scan_goes_on(void)
{
    const holo_motion_t scan = { .kind = HOLO_SCAN, .range = 40, .period_ms = 2000, .duration_ms = -1 };
    start(&scan);
    CHECK(frames_until_hold(10000) == -1);
    CHECK(strcmp(motion_now(), "scan") == 0);
    const holo_motion_t stop = { .kind = HOLO_STOP, .duration_ms = -1 };
    start(&stop);
    CHECK(strcmp(motion_now(), "hold") == 0);
}

/* A timed twitch ends at its limit too, glance in flight or not */
static void test_timed_twitch_ends(void)
{
    const holo_motion_t twitch = { .kind = HOLO_TWITCH, .range = 30, .interval_min_s = 0.5f, .interval_max_s = 1.5f,
                                   .time_s = 2.5f, .duration_ms = -1 };
    start(&twitch);
    const int64_t took = frames_until_hold(10000);
    CHECK(took >= 2500 && took <= 2500 + TICK_MS);
    const int before = moves();
    for (int i = 0; i < 200; i++) {
        frame();
    }
    CHECK(moves() == before);
}

/* The motions that end by themselves still do, untouched by the limit */
static void test_wag_ends(void)
{
    const holo_motion_t wag = { .kind = HOLO_WAG, .count = 2, .period_ms = 600, .duration_ms = -1 };
    start(&wag);
    const int64_t took = frames_until_hold(10000);
    /* a quarter period out, three half-period swings, a quarter period home: 2 x 600 ms, a frame each leg at most */
    CHECK(took >= 1200 && took <= 1200 + 5 * TICK_MS);
}

int main(void)
{
    static const holo_desc_t holos[] = { { .name = "front", .h = "h", .v = "v", .led = NULL } };
    const holo_config_t cfg = { .axis = hook_axis, .axis_move = hook_move };
    s_now_us = 1000000;
    CHECK(holo_start(holos, 1, &cfg) == ESP_OK);

    test_timed_scan_ends();
    test_timed_scan_mid_leg();
    test_untimed_scan_goes_on();
    test_timed_twitch_ends();
    test_wag_ends();

    printf("test_holo: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
