/*
 * The C face of the servo component: what the rest of the application, which is C, calls.
 * The classes behind it are in the .hpp headers alongside.
 *
 * Nothing here knows what is wired where. The application decides which boards and pins
 * exist and what their servos are called, and attaches them through servo_attach_pca9685()
 * and servo_attach_gpio(); this file only offers the building blocks.
 *
 * Every ident is a string the application chose at attach time ("pca0_3"). A calibration
 * is saved under the board address and channel, or the pin, instead, so it follows the wiring rather
 * than the name.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One servo's limits, in microseconds of pulse width. */
typedef struct {
    uint16_t abs_min_us;    /**< what the hardware may ever be sent */
    uint16_t abs_max_us;
    uint16_t op_min_us;     /**< the working range a percentage maps onto */
    uint16_t op_max_us;
    bool invert;            /**< 0% is op_max rather than op_min */
    bool calibrated;        /**< the working range was set and saved, rather than defaulted
                                 to the absolute one */
} servo_limits_t;

/**
 * @brief Attach a PCA9685 at `address` on `i2c_port` and register its sixteen channels as
 *        "<prefix>0" .. "<prefix>15", each taking abs_min_us..abs_max_us.
 *
 * Probes before attaching, so a board that is not fitted costs one probe and no controller.
 * Idempotent per address: a board already attached is left alone, so this can be called
 * again to pick up one plugged in after boot. The servo task starts with the first board
 * that answers.
 *
 * Needs the bus to exist: the port's i2c_master bus is borrowed, never created here.
 *
 * @return ESP_OK when the board is attached, now or already; ESP_ERR_INVALID_STATE when
 *         there is no I2C master bus on that port yet; ESP_ERR_NOT_FOUND when nothing
 *         answered at the address; ESP_FAIL when it answered and refused its setup.
 */
esp_err_t servo_attach_pca9685(int i2c_port, uint8_t address, const char *prefix,
                               uint16_t abs_min_us, uint16_t abs_max_us);

/**
 * @brief Register a servo on one of the chip's own pins, driven by MCPWM, as `ident`.
 *
 * Each takes a whole MCPWM timer: six on an S3. The working range starts as
 * op_min_us..op_max_us -- the application's idea of the travel -- and reads as uncalibrated
 * until servo_set_op_limits() saves one; servo_clear_op_limits() comes back to it. The
 * calibration and drive policy are saved under "gpio<N>", so they follow the wiring. The
 * line stays low, the servo limp, until the first move or enable.
 *
 * Idempotent per pin: a pin already attached is left alone, so this can run again.
 *
 * @return ESP_OK when the servo is attached, now or already; ESP_ERR_INVALID_ARG for a
 *         range that is not abs_min <= op_min <= op_max <= abs_max, or an ident already
 *         taken; ESP_FAIL when the MCPWM refused (no timer free, or the pin; logged).
 */
esp_err_t servo_attach_gpio(int gpio, const char *ident, uint16_t abs_min_us, uint16_t abs_max_us,
                            uint16_t op_min_us, uint16_t op_max_us);

/** @brief How many controllers (boards) are attached. */
size_t servo_controller_count(void);

/**
 * @brief Run every attached controller's setup sequence again, with all channels off.
 *
 * For a board whose logic supply dropped and came back: a PCA9685 wakes with its power-on
 * prescaler, and every frame after that is at the wrong rate until this runs. The servos
 * go limp; the caller re-enables whatever it wants driven.
 */
esp_err_t servo_reset_controllers(void);

/** @brief Whether a servo with this ident is registered. */
bool servo_exists(const char *id);

/**
 * @brief Power a servo's channel on or off. Off means no pulses: the servo goes limp.
 * @return false if no servo has that ident.
 */
bool servo_set_enable(const char *id, bool enable);

/**
 * @brief Enable a servo and move it to a point in its working range.
 * @param percentage 0..1000, in tenths of a percent; larger values are clamped.
 * @return false if no servo has that ident.
 */
bool servo_set_percentage(const char *id, uint16_t percentage);

/**
 * @brief Move a servo to a raw pulse width, clamped to its absolute range.
 *
 * Does not enable it: a disabled channel takes the value and stays limp until enabled, so
 * a caller can set where a servo will go before switching it on. Reaches the wire at the
 * servo task's next 20 ms frame rather than immediately, which is what makes this cheap
 * enough to call for every step of a drag.
 *
 * @return false if no servo has that ident.
 */
bool servo_move_us(const char *id, uint16_t us);

/**
 * @brief The pulse width last commanded, in microseconds -- the absolute range's centre if
 *        nothing has moved it. A PWM servo reports nothing back: this is where it was sent,
 *        not where it is, and says nothing once the output is released.
 * @return false if no servo has that ident.
 */
bool servo_get_value_us(const char *id, uint16_t *us);

/** @brief Read a servo's limits. @return false if no servo has that ident. */
bool servo_get_limits(const char *id, servo_limits_t *out);

/**
 * @brief Set and save a servo's working range.
 * @return false if no servo has that ident, the range is outside the absolute one,
 *         min > max, or the save failed.
 */
bool servo_set_op_limits(const char *id, uint16_t min_us, uint16_t max_us, bool invert);

/** @brief Forget a servo's saved working range and drive policy: back to the range it was
 *         attached with (the absolute one, for a PCA9685 channel), HOLD everywhere,
 *         uncalibrated. */
bool servo_clear_op_limits(const char *id);

/* ---- a channel as plain PWM ------------------------------------------------------ */

/** @brief servo_set_duty()'s full scale: the channel on for the whole frame. */
#define SERVO_DUTY_FULL 4096

/**
 * @brief Drive a PCA9685 channel as a plain PWM output -- an LED on a servo header --
 *        rather than as a servo.
 *
 * `duty` is out of SERVO_DUTY_FULL: 0 is off, SERVO_DUTY_FULL is on for the whole frame.
 * The frame rate is the servos' 50 Hz, the only rate the chip has, so a dimmed LED is
 * pulsed at 50 Hz. From the first call the channel belongs to this call: the servo of the
 * same ident still exists and still takes commands, but none of them reach the wire until
 * servo_release_duty(). A controller reset (servo_reset_controllers) turns the channel off
 * and leaves it claimed.
 *
 * @return false if the ident is not a PCA9685 channel -- which includes one whose board has
 *         not answered yet. Unlike the servo calls, that is not logged.
 */
bool servo_set_duty(const char *id, uint16_t duty);

/** @brief Hand a channel servo_set_duty() claimed back to its servo. */
bool servo_release_duty(const char *id);

/** @brief The duty of a channel servo_set_duty() claimed; false for any other. */
bool servo_get_duty(const char *id, uint16_t *duty);

/* ---- the drive policy (docs/servo_model_spec.md section 3.4) ------------------ */

#define SERVO_DRIVE_HOLD    0
#define SERVO_DRIVE_RELEASE 1

/** Whether the output stage keeps driving once a command has settled, per zone, and how
 *  long it is driven after any command before a RELEASE takes effect. Zones are named by
 *  the commanded value against the working range, 2% of travel wide at each end. */
typedef struct {
    uint8_t drive_closed;   /**< SERVO_DRIVE_HOLD or SERVO_DRIVE_RELEASE */
    uint8_t drive_open;
    uint8_t drive_mid;
    uint16_t settle_ms;
} servo_drive_policy_t;

/** @brief Read a servo's drive policy; HOLD everywhere with no settle time when nothing
 *         was ever saved. @return false if no servo has that ident. */
bool servo_get_drive_policy(const char *id, servo_drive_policy_t *out);

/** @brief Set and save a servo's drive policy. Takes effect at the next frame. A settle
 *         time under 100 ms is raised to 100: shorter, the output dropped before the
 *         horn had moved. */
bool servo_set_drive_policy(const char *id, const servo_drive_policy_t *policy);

/** @brief Drive a servo's output now and keep driving it until its next move, whatever
 *         the policy says -- MoveServoRaw's ENABLE_ON. servo_set_enable(id, true) drives
 *         it too, but the policy applies again once the settle time has run. */
bool servo_hold(const char *id);

/** @brief The output stage right now: `driven` is whether pulses are going out,
 *         `released` whether the policy is what stopped them -- the state a UI would
 *         show as RELEASED, distinct from a servo that was switched off. */
bool servo_get_output(const char *id, bool *driven, bool *released);

/**
 * @brief Be told when a servo's output changes on its own: the policy releasing a settled
 *        servo, or a command re-driving a released one. Called from the servo task with
 *        no lock held, so the callback may call anything, including back into this API.
 *        One listener; a later call replaces it.
 */
typedef void (*servo_output_cb_t)(const char *id, bool driven, bool released, void *ctx);
void servo_set_output_callback(servo_output_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
