/* Host stub for test_holo: the test's clock (stub_now_us, test_holo.c), which it moves itself */
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
