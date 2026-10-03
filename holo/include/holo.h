/*
 * holo: a holoprojector's motions -- a horizontal and a vertical servo axis, and optionally a
 * light -- and the `holo` console command.
 *
 * Motions: center, move, nudge, twitch (random glances), wag (side to side), nod (up and
 * down), scan (a slow sweep), circle, stop. Light modes: off, on, pulse, flicker, blink, each
 * optionally timed, and leia (centre and flicker). All run on one task at the servos' 20 ms
 * frame.
 *
 * The axes are servo idents (the servo component). By default the engine moves them straight
 * through servo.h; an application with its own servo registry or an arm switch hands in hooks
 * (holo_config_t) and the engine goes through those instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a motion was refused, or stopped. Hooks report theirs with these where they fit. */
#define HOLO_WHY_NOT_ARMED    "not armed"
#define HOLO_WHY_UNKNOWN      "unknown axis"
#define HOLO_WHY_NOT_FITTED   "not fitted"
#define HOLO_WHY_UNCALIBRATED "uncalibrated"
/* And why a light command was: the holo was described without one. */
#define HOLO_WHY_NO_LIGHT     "no light"

/** One holoprojector. The strings must outlive the engine. */
typedef struct {
    const char *name;   /**< its name on the console: "rear", "top", "front" */
    const char *h;      /**< the horizontal axis's servo ident */
    const char *v;      /**< the vertical axis's servo ident */
    const char *led;    /**< a servo channel driven as plain PWM (servo_set_duty()) for its
                             light, or NULL for a holo with none */
} holo_desc_t;

/**
 * An axis as the engine sees it. `permille` is 0 at CLOSED -- full left, or down -- and
 * 1000 at OPEN, full right or up.
 */
typedef struct {
    bool fitted;            /**< the servo exists */
    bool calibrated;        /**< its endpoints are set, so a percentage means something */
    bool placed;            /**< where it is is known: `permille` and `us` mean something */
    uint16_t permille;
    uint16_t closed_us;
    uint16_t open_us;
    uint16_t us;
    const char *display;    /**< a note for `holo` status, or NULL */
} holo_axis_t;

/**
 * How the engine reaches its axes. Every member may be NULL, and so may the whole config:
 * then nothing is ever disarmed, an axis is a servo ident moved with servo_set_percentage(),
 * and its endpoints are the servo's saved working range (servo_set_op_limits()).
 *
 * The hooks are called with the engine's lock held, from the console task and from the
 * engine's task, whose stack may be in PSRAM: a hook must not write flash from the engine's
 * task. `axis` returning false means the ident is unknown.
 */
typedef struct {
    bool (*armed)(void *ctx);
    bool (*axis)(const char *ident, holo_axis_t *out, void *ctx);
    bool (*axis_move)(const char *ident, uint16_t permille, const char **why, void *ctx);
    bool (*endpoints)(const char *ident, bool clear, uint16_t closed_us, uint16_t open_us,
                      const char **why, void *ctx);
    /** Stop driving an axis, so it goes limp (`holo off`); the next motion drives it again.
     *  NULL: servo_set_enable(ident, false). */
    void (*axis_release)(const char *ident, void *ctx);
    void *ctx;
    /**
     * With the default hooks: move a servo that has no saved calibration, over the working
     * range it was registered with. For servos whose defaults are their real travel; leave
     * false where the defaults are only the absolute range.
     */
    bool allow_uncalibrated;
} holo_config_t;

/**
 * Start the engine on `count` holos. `holos` must outlive it; `config` is copied and may be
 * NULL. The servos need not exist yet: an axis that is not fitted refuses motion until it is.
 */
esp_err_t holo_start(const holo_desc_t *holos, size_t count, const holo_config_t *config);

/** The `holo` console command. `holo help` prints the arguments. */
void holo_register_command(void);

/* ---- for other callers (an HTTP API): what the console's verbs do ------------- */

typedef enum {
    HOLO_CENTER, HOLO_MOVE, HOLO_NUDGE,             /* to a place */
    HOLO_TWITCH, HOLO_SCAN,                         /* until stopped, or for time_s */
    HOLO_WAG, HOLO_NOD, HOLO_CIRCLE,                /* `count` times, and back */
    HOLO_STOP,                                      /* hold where it is */
    HOLO_OFF,                                       /* stop, the axes limp, the light off */
} holo_motion_kind_t;

/** A motion, as the console's options give it. 0 (or -1 for duration_ms) is the default. */
typedef struct {
    holo_motion_kind_t kind;
    int x, y;                   /**< move: -100..100 of the way to an endpoint; nudge: -200..200 */
    int duration_ms;            /**< center, move, nudge: 0..10000; -1 by the distance */
    int range;                  /**< 1..100 percent of the half-travel */
    int count;                  /**< wag, nod: cycles; circle: turns; 1..20 */
    int period_ms;              /**< 200..60000 */
    float interval_min_s, interval_max_s;   /**< twitch: the pause between glances, 0.1..600 */
    float time_s;               /**< twitch, scan: stop after this long, 0.1..3600 */
} holo_motion_t;

/**
 * Start a motion on holo `idx`, replacing the one running. ESP_ERR_INVALID_ARG for a
 * parameter out of range, ESP_ERR_INVALID_STATE when the holo cannot move (not fitted, not
 * armed, uncalibrated), ESP_ERR_NOT_FOUND for no such holo -- each with `why`.
 */
esp_err_t holo_motion(size_t idx, const holo_motion_t *m, char *why, size_t why_len);

/** Whether `m`'s parameters are within range, without starting it: false with `why`. */
bool holo_motion_check(const holo_motion_t *m, char *why, size_t why_len);

/* The light's modes, in the console's order: `holo <holo> led off|on|pulse|flicker|blink`. */
typedef enum {
    HOLO_LIGHT_OFF, HOLO_LIGHT_ON, HOLO_LIGHT_PULSE, HOLO_LIGHT_FLICKER, HOLO_LIGHT_BLINK,
} holo_light_mode_t;

typedef struct {
    holo_light_mode_t mode;
    uint8_t level;              /**< 0..100 percent; meaningless for OFF */
    uint32_t period_ms;         /**< pulse, blink: 200..60000; 0 the mode's default (2000, 1000) */
} holo_light_t;

/**
 * Set holo `idx`'s light, as `holo <holo> led ...` does. `time_s` > 0 makes it a timed
 * mode that goes back to what the light was doing after that long (`-t`); 0 is for good.
 * ESP_ERR_INVALID_ARG for a level, period or time out of range, ESP_ERR_INVALID_STATE
 * when the holo has no light or its channel is not there (HOLO_WHY_NO_LIGHT,
 * HOLO_WHY_NOT_FITTED with the channel: "not fitted (pca1_0)"), ESP_ERR_NOT_FOUND for no
 * such holo -- each with `why`, which carries no name prefix.
 */
esp_err_t holo_light(size_t idx, const holo_light_t *light, float time_s, char *why, size_t why_len);

/**
 * The message from Leia, as `holo <holo> leia` does it: the light flickers at full for
 * `time_s` (0: the default 30 s) and then goes back to what it was, and the holo looks
 * straight out if it may. ESP_OK once the light half ran, with `*centred` saying whether
 * the centre did too; when it did not, `why` is the centre's refusal ("uncalibrated
 * (pca1_2)"). ESP_ERR_INVALID_STATE with `why` when the light half itself could not run,
 * ESP_ERR_INVALID_ARG for a time out of range, ESP_ERR_NOT_FOUND for no such holo.
 */
esp_err_t holo_leia(size_t idx, float time_s, bool *centred, char *why, size_t why_len);

typedef struct {
    const char *name;
    bool ready;                 /**< it could move now */
    const char *why;            /**< a HOLO_WHY_* when it could not; NULL when ready */
    const char *why_axis;       /**< the axis at fault, if one is */
    const char *motion;         /**< "hold", "move", "twitch", "wag", "nod", "scan", "circle" */
    const char *stopped;        /**< why the last motion ended early, or NULL */
    bool has_light;
    bool placed;                /**< x, y are where the axes were sent; else where the engine thinks */
    float x, y;                 /**< -1..1: + is right, and up */

    /* The motion in time. A panel draws a countdown from these, so they are spans from
     * now rather than stamps: the caller's clock need not be the engine's. */
    uint32_t motion_elapsed_ms;     /**< since the running motion began; 0 when holding */
    uint32_t motion_remaining_ms;   /**< until a timed twitch or scan ends; UINT32_MAX when
                                         there is no limit, or nothing runs */
    /* The stop, when `stopped` is set: which motion it was, the axis the hook refused (or
     * NULL, for a disarm), and how long ago. 0 and NULL otherwise. */
    const char *stopped_motion;
    const char *stopped_axis;
    uint32_t stopped_ms_ago;

    /* The light. `light_fitted` is `has_light` and its channel's board answering. */
    bool light_fitted;
    holo_light_t light;             /**< what it is doing now */
    uint32_t light_remaining_ms;    /**< a timed mode: until it goes back; UINT32_MAX when not timed */
    holo_light_t light_after;       /**< what it goes back to; meaningful when timed */
    bool leia;                      /**< the light is a leia's flicker, from any caller, until it
                                         ends or another light command replaces it */
} holo_status_t;

esp_err_t holo_status(size_t idx, holo_status_t *out);

/** The description holo `idx` was started with (its name and channels); false past the end. */
bool holo_describe(size_t idx, holo_desc_t *out);

/** The last motion asked of holo `idx` -- by the console or any caller -- as it was asked; before
 *  any, a HOLO_STOP. With holo_status(), what it takes to put a holo back to what it was doing. */
bool holo_last_motion(size_t idx, holo_motion_t *out);

/** Holos the engine runs (0 before holo_start()); their names. */
size_t holo_count(void);
const char *holo_name(size_t idx);

#ifdef __cplusplus
}
#endif
