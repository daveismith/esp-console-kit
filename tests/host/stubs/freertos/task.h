/* Host stub for test_holo: a task is made and never runs */
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t f, const char *n, uint32_t st, void *a, int p, TaskHandle_t *h,
                                             int caps)
{
    static int task;
    (void)f; (void)n; (void)st; (void)a; (void)p; (void)caps;
    *h = &task;
    return pdPASS;
}
static inline BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *a, int p, TaskHandle_t *h)
{
    return xTaskCreateWithCaps(f, n, st, a, p, h, 0);
}
static inline void xTaskNotifyGive(TaskHandle_t h) { (void)h; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelayUntil(TickType_t *last, TickType_t inc) { *last += inc; }
