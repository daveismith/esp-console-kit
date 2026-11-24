/* Host stub: what the holo engine (holo/holo.c) uses of esp_err.h, for test_holo */
#pragma once
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERROR_CHECK(x)    do { if ((x) != ESP_OK) abort(); } while (0)
