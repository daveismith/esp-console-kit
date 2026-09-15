/*
 * XMODEM-CRC / XMODEM-1K, both directions, over any byte transport.
 *
 * Receive accepts 128-byte (SOH) and 1024-byte (STX) blocks with CRC-16, requested with 'C'.
 * Send uses 1024-byte blocks with CRC-16 when the receiver asks with 'C', and 128-byte
 * blocks with the 8-bit checksum when it asks with NAK.
 *
 * Plain XMODEM carries no length: the last block is padded with SUB (0x1A). A receiver told
 * the size delivers exactly that many bytes; one that is not strips trailing SUBs from the
 * last block, which is wrong for a file that really ends in 0x1A -- so pass the size.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Read up to `len` bytes, returning once `len` have arrived or `timeout_ms` has passed.
     * Returns the number read (0 on timeout). */
    int (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
    /* Write all of `len` bytes. Returns the number written. */
    int (*write)(void *ctx, const uint8_t *buf, size_t len);
    /* Discard anything already received. */
    void (*purge)(void *ctx);
    void *ctx;
} xmodem_io_t;

/* Receives payload bytes in order. An error cancels the transfer. */
typedef esp_err_t (*xmodem_sink_t)(void *ctx, const uint8_t *data, size_t len);
/* Fills `buf` with up to `len` payload bytes: the count produced, 0 at the end, <0 on error. */
typedef int (*xmodem_source_t)(void *ctx, uint8_t *buf, size_t len);

typedef struct {
    uint32_t start_timeout_ms;   /* waiting for the other end to start; 0 = 60 s */
    size_t expected_size;        /* receive only: the payload length, 0 = unknown */
    /* send only, CRC mode: 1024 (default) or 128. Small blocks pace the stream by the ACK
     * round trip -- for a USB-serial bridge whose host driver drains more slowly than the
     * line runs, a 1K burst at 921600+ baud overflows the bridge and is lost. */
    size_t block_size;
} xmodem_config_t;

typedef struct {
    size_t bytes;                /* payload bytes delivered or sent */
    unsigned blocks;
    unsigned retries;            /* blocks resent or NAKed */
    unsigned timeouts;           /* receive: blocks whose body did not arrive in time */
    unsigned bad_blocks;         /* receive: block-number complement or CRC mismatches */
    unsigned skipped;            /* receive: stray bytes skipped where a header belonged */
    /* receive, the first bad block: how much of it arrived, its first bytes, and the CRCs */
    size_t diag_len;
    uint8_t diag_head[8];
    uint16_t diag_crc_rx, diag_crc_calc;
} xmodem_stats_t;

/*
 * Returns ESP_OK, or:
 *   ESP_ERR_TIMEOUT          the other end never started, or went quiet
 *   ESP_ERR_INVALID_STATE    the other end cancelled (CAN CAN)
 *   ESP_ERR_INVALID_RESPONSE blocks out of sequence
 *   ESP_ERR_INVALID_SIZE     receive: fewer bytes than expected_size
 *   ESP_FAIL                 too many bad blocks
 *   or the sink's / source's own error.
 * On any failure the other end is sent CAN CAN.
 */
esp_err_t xmodem_receive(const xmodem_io_t *io, const xmodem_config_t *config,
                         xmodem_sink_t sink, void *sink_ctx, xmodem_stats_t *stats);
esp_err_t xmodem_send(const xmodem_io_t *io, const xmodem_config_t *config,
                      xmodem_source_t source, void *source_ctx, xmodem_stats_t *stats);

/* CRC-16/XMODEM (polynomial 0x1021, initial value 0). */
uint16_t xmodem_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
