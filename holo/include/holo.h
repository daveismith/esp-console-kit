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

#ifdef __cplusplus
}
#endif
