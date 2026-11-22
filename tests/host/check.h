/*
 * The host tests' one assertion: CHECK(cond) counts it, and prints the line of one that fails, without
 * stopping. Each test file's main() reports s_checks and s_failed and returns non-zero on a failure.
 */
#pragma once

#include <stdio.h>

static int s_failed, s_checks;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        s_checks++;                                                                                 \
        if (!(cond)) {                                                                              \
            s_failed++;                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
        }                                                                                           \
    } while (0)
