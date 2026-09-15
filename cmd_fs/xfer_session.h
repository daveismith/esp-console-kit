/*
 * A transfer's hold on the UART: raw, with logging muted, optionally at another baud rate.
 * Shared by `fs put` / `fs get` and `ota put`. Private to this component.
 */
#pragma once

#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "xmodem.h"

typedef struct {
    uart_port_t port;
    uint32_t console_baud;
    uint32_t baud;
    vprintf_like_t saved_log;
    xmodem_io_t io;
} xfer_session_t;

/* The rate the console runs at on `uart_num` (-1: the console UART), for the ready line;
 * 0 when there is no such UART. */
uint32_t xfer_console_baud(int uart_num);

/* Take the UART for a transfer at `baud` (0: the console's rate). Call it once the ready
 * line is printed: that line goes out at the console's rate, and the host switches rate
 * when it has it. */
esp_err_t xfer_session_begin(xfer_session_t *s, int uart_num, uint32_t baud);
void xfer_session_end(xfer_session_t *s);

/* An XMODEM failure, in words. */
const char *xfer_err(esp_err_t err);
