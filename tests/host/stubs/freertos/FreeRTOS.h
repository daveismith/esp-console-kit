/* Host stub for test_holo: no scheduler. The engine's task is never run; the test calls its frame itself */
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdPASS  1
#define pdTRUE  1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
