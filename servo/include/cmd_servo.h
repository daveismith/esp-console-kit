/*
 * Console commands for the servos: servo_list, servo_register, servo_move, servo_sweep,
 * servo_config.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The application's "attach whatever is on the bus". This component does not know which
 * boards exist or what to call their channels; the application does, and hands the
 * function over here so that `servo_register` can run it and every other command can run
 * it once if nothing has been attached yet. Returns as servo_attach_pca9685() does:
 * ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_STATE before the bus exists.
 */
typedef esp_err_t (*servo_attach_fn_t)(void);

/** Register the commands. `attach` may be NULL, in which case servo_register says so. */
void register_servo(servo_attach_fn_t attach);

#ifdef __cplusplus
}
#endif
