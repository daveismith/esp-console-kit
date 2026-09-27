/*
 * web_servo -- the servos over HTTP: the /api/v1/servos routes, over servo.h. See web_servo.c.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the routes. `before_move`, if not NULL, is called before a servo is driven directly
 * (POST /servos/move) -- to stop whatever else drives it, such as a holo's motion.
 */
esp_err_t web_servo_register(void (*before_move)(void));

#ifdef __cplusplus
}
#endif
