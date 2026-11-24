/* Host stub for test_holo */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT   2
static inline void *heap_caps_calloc_prefer(size_t n, size_t size, int count, ...) { (void)count; return calloc(n, size); }
