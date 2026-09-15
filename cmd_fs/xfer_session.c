/*
 * XMODEM over the console UART: the session a transfer runs in. See xfer_session.h.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "xfer_session.h"

/* RX FIFO fill level that wakes the UART ISR: during a transfer, and the driver's default. */
#define RX_FULL_THRESHOLD         64
#define RX_FULL_THRESHOLD_CONSOLE 120

#if defined(CONFIG_ESP_CONSOLE_UART_NUM) && CONFIG_ESP_CONSOLE_UART_NUM >= 0
#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#else
#define CONSOLE_UART (-1)
#endif

static int uart_io_read(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    const int n = uart_read_bytes((uart_port_t)(intptr_t)ctx, buf, len, pdMS_TO_TICKS(timeout_ms));
    return n < 0 ? 0 : n;
}

static int uart_io_write(void *ctx, const uint8_t *buf, size_t len)
{
    return uart_write_bytes((uart_port_t)(intptr_t)ctx, buf, len);
}

static void uart_io_purge(void *ctx)
{
    uart_flush_input((uart_port_t)(intptr_t)ctx);
}

static int discard_vprintf(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

uint32_t xfer_console_baud(int uart_num)
{
    const int port = uart_num >= 0 ? uart_num : CONSOLE_UART;
    uint32_t baud = 0;
    if (port >= 0) {
        uart_get_baudrate((uart_port_t)port, &baud);
    }
    return baud;
}

/*
 * A transfer takes the console UART raw: straight to the driver, past the VFS, whose line
 * ending translation (CR -> LF in, LF -> CRLF out) would corrupt binary data. Log output from
 * other tasks is silenced for the duration, since a line of it in the middle of a block is a
 * CRC failure at best. The ready line has already gone out at the console's own rate; the
 * host switches rate once it has that line, so the switch here waits for it to leave.
 */
esp_err_t xfer_session_begin(xfer_session_t *s, int uart_num, uint32_t baud)
{
    const int port = uart_num >= 0 ? uart_num : CONSOLE_UART;
    if (port < 0 || !uart_is_driver_installed((uart_port_t)port)) {
        printf("xmodem: no UART driver on the console; XMODEM needs one\n");
        return ESP_ERR_INVALID_STATE;
    }
    s->port = (uart_port_t)port;
    s->io = (xmodem_io_t) {
        .read = uart_io_read, .write = uart_io_write, .purge = uart_io_purge,
        .ctx = (void *)(intptr_t)port,
    };
    uart_get_baudrate(s->port, &s->console_baud);
    s->baud = baud ? baud : s->console_baud;
    fflush(stdout);
    uart_wait_tx_done(s->port, pdMS_TO_TICKS(1000));
    s->saved_log = esp_log_set_vprintf(discard_vprintf);
    /* The RX FIFO is 128 bytes and the driver empties it at 120: 8 bytes of slack, under
     * 90 us at 921600 baud, and an overflow resets the FIFO -- a block lost ~64 bytes that
     * way at 921600 and 2M. At 64 there is room for 64 more. */
    uart_set_rx_full_threshold(s->port, RX_FULL_THRESHOLD);
    if (s->baud != s->console_baud) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_err_t err = uart_set_baudrate(s->port, s->baud);
        if (err != ESP_OK) {
            esp_log_set_vprintf(s->saved_log);
            printf("xmodem: cannot run the UART at %" PRIu32 " baud\n", s->baud);
            return err;
        }
    }
    return ESP_OK;
}

void xfer_session_end(xfer_session_t *s)
{
    uart_wait_tx_done(s->port, pdMS_TO_TICKS(1000));
    /* The host takes the last ACK and drops back to the console rate; anything said here
     * before it has would arrive as garbage. */
    vTaskDelay(pdMS_TO_TICKS(300));
    if (s->baud != s->console_baud) {
        uart_set_baudrate(s->port, s->console_baud);
    }
    uart_set_rx_full_threshold(s->port, RX_FULL_THRESHOLD_CONSOLE);
    uart_flush_input(s->port);
    esp_log_set_vprintf(s->saved_log);
}

const char *xfer_err(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:          return "timed out (is the sender/receiver running?)";
    case ESP_ERR_INVALID_STATE:    return "cancelled by the other end";
    case ESP_ERR_INVALID_RESPONSE: return "blocks out of sequence";
    case ESP_ERR_INVALID_SIZE:     return "fewer bytes than announced";
    case ESP_FAIL:                 return "too many errors";
    default:                       return esp_err_to_name(err);
    }
}
