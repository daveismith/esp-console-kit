/* Host stub for test_holo: the command is registered nowhere */
#pragma once
#include "esp_err.h"
typedef struct {
    const char *command, *help, *hint;
    int (*func)(int argc, char **argv);
    void *argtable;
} esp_console_cmd_t;
static inline esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd) { (void)cmd; return ESP_OK; }
