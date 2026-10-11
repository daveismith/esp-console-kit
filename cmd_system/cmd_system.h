/* Console example — various system commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#include <stdbool.h>
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

/*
 * Called with `true` just before `tasks` or `top` walks the task list, and with `false` just
 * after. The walk is uxTaskGetSystemState(), which holds the kernel's lock -- interrupts off
 * on the calling core, and the other core's tick spinning on it -- while it scans every
 * task's stack for the fill pattern: ~2 ms on a board with PSRAM stacks. An application
 * with a peripheral that cannot miss an interrupt for that long (a strip refilled from an
 * ISR) quiets it here. NULL removes the hook. Not called from an interrupt.
 */
typedef void (*cmd_system_task_walk_hook_t)(bool before, void *ctx);
void cmd_system_set_task_walk_hook(cmd_system_task_walk_hook_t hook, void *ctx);

// Register deep and light sleep functions
void register_system_deep_sleep(void);
void register_system_light_sleep(void);

#ifdef __cplusplus
}
#endif
