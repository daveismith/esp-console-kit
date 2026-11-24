/*
 * The host tests' register values of each kind, as a tool would send them: nat16(n, a, b, ...) with n values, nat32(n,
 * a, b, c) with the first n of three, str(s, len). Inline, so a test that uses one of them builds without the others.
 */
#pragma once

#include <stdarg.h>
#include <string.h>
#include "uavcan/_register/Value_1_0.h"

static inline uavcan_register_Value_1_0 nat16(int n, ...)
{
    uavcan_register_Value_1_0 v;
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_natural16_(&v);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        v.natural16.value.elements[i] = (uint16_t)va_arg(ap, int);
    }
    va_end(ap);
    v.natural16.value.count = (size_t)n;
    return v;
}

static inline uavcan_register_Value_1_0 nat32(int n, uint32_t a, uint32_t b, uint32_t c)
{
    uavcan_register_Value_1_0 v;
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_natural32_(&v);
    const uint32_t e[3] = { a, b, c };
    memcpy(v.natural32.value.elements, e, sizeof(e));
    v.natural32.value.count = (size_t)n;
    return v;
}

static inline uavcan_register_Value_1_0 str(const char *s, size_t len)
{
    uavcan_register_Value_1_0 v;
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_string_(&v);
    memcpy(v._string.value.elements, s, len);
    v._string.value.count = len;
    return v;
}
