/* Console example — various system commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register all system functions
void register_system(void);

// Register common system functions: "version", "restart", "free", "heap", "tasks"
void register_system_common(void);

/* One pin the `gpio` command must never drive, and why (shown to whoever tries). */
typedef struct {
    int8_t pin;
    const char *why;
} gpio_reserved_t;

/* Register `gpio get|set|release`. `reserved` must outlive the console. */
void register_gpio(const gpio_reserved_t *reserved, size_t count);

// Register deep and light sleep functions
void register_system_deep_sleep(void);
void register_system_light_sleep(void);

#ifdef __cplusplus
}
#endif
