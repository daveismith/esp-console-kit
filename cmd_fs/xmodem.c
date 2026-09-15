/*
 * XMODEM-CRC / XMODEM-1K. See xmodem.h.
 *
 * References: the XMODEM/YMODEM protocol reference (Chuck Forsberg, 1988) and
 * https://www.menie.org/georges/embedded/xmodem_specification.html.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "xmodem.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1A
#define CRC_REQUEST 'C'

#define MAX_BLOCK         1024
#define MAX_ERRORS        10
#define MAX_NOISE         (4 * MAX_BLOCK)  /* stray bytes tolerated where a header belongs */
#define START_TIMEOUT_MS  60000
#define POLL_MS           1000     /* receiver: 'C' this often until the sender starts */
#define BLOCK_TIMEOUT_MS  2000     /* the rest of a block once its header has arrived */
#define HEADER_TIMEOUT_MS 10000    /* between blocks */
#define REPLY_TIMEOUT_MS  10000    /* sender: waiting for ACK/NAK */
#define QUIET_MS          100      /* line idle this long before a NAK */
#define EOT_LINGER_MS     2000     /* receiver: answering repeated EOTs after the first */

uint16_t xmodem_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool read_byte(const xmodem_io_t *io, uint8_t *c, uint32_t timeout_ms)
{
    return io->read(io->ctx, c, 1, timeout_ms) == 1;
}

/* The next byte before `deadline`. Waiting is by the clock, not by the number of reads, so
 * noise on the line (a baud rate change leaves some) cannot use up a timeout early. */
static bool read_byte_by(const xmodem_io_t *io, uint8_t *c, int64_t deadline)
{
    const int64_t left = deadline - now_ms();
    return left > 0 && read_byte(io, c, (uint32_t)left);
}

static bool read_exact(const xmodem_io_t *io, uint8_t *buf, size_t len, uint32_t timeout_ms,
                       size_t *got_out)
{
    const int64_t deadline = now_ms() + timeout_ms;
    size_t got = 0;
    bool ok = true;
    while (got < len) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            ok = false;
            break;
        }
        const int n = io->read(io->ctx, buf + got, len - got, (uint32_t)left);
        if (n > 0) {
            got += (size_t)n;
        }
    }
    if (got_out) {
        *got_out = got;
    }
    return ok;
}

static void send_byte(const xmodem_io_t *io, uint8_t c)
{
    io->write(io->ctx, &c, 1);
}

static void send_cancel(const xmodem_io_t *io)
{
    static const uint8_t cancel[] = { CAN, CAN, CAN };
    io->write(io->ctx, cancel, sizeof(cancel));
}

/* Wait out whatever is still arriving -- the rest of a mangled block -- so a NAK is not
 * answered with the tail of the block it condemns. */
static void wait_quiet(const xmodem_io_t *io)
{
    uint8_t scratch[64];
    while (io->read(io->ctx, scratch, sizeof(scratch), QUIET_MS) > 0) {
    }
    if (io->purge) {
        io->purge(io->ctx);
    }
}

/* ------------------------------------------------------------------ receive */

typedef struct {
    const xmodem_config_t *cfg;
    xmodem_sink_t sink;
    void *ctx;
    size_t delivered;
} rx_state_t;

/* A block is held back until the next header shows whether it was the last: only then may
 * its padding be trimmed. */
static esp_err_t deliver(rx_state_t *st, const uint8_t *data, size_t len, bool last)
{
    if (st->cfg->expected_size) {
        const size_t remaining = st->cfg->expected_size > st->delivered
                               ? st->cfg->expected_size - st->delivered : 0;
        if (len > remaining) {
            len = remaining;
        }
    } else if (last) {
        while (len > 0 && data[len - 1] == SUB) {
            len--;
        }
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = st->sink(st->ctx, data, len);
    if (err == ESP_OK) {
        st->delivered += len;
    }
    return err;
}

esp_err_t xmodem_receive(const xmodem_io_t *io, const xmodem_config_t *config,
                         xmodem_sink_t sink, void *sink_ctx, xmodem_stats_t *stats)
{
    const xmodem_config_t defaults = { 0 };
    const xmodem_config_t *cfg = config ? config : &defaults;
    const uint32_t start_timeout = cfg->start_timeout_ms ? cfg->start_timeout_ms : START_TIMEOUT_MS;
    xmodem_stats_t local = { 0 };
    xmodem_stats_t *s = stats ? stats : &local;
    memset(s, 0, sizeof(*s));

    /* 2 block-number bytes + data + 2 CRC bytes, and the held block. */
    uint8_t *frame = malloc(2 + MAX_BLOCK + 2);
    uint8_t *held = malloc(MAX_BLOCK);
    if (frame == NULL || held == NULL) {
        free(frame);
        free(held);
        return ESP_ERR_NO_MEM;
    }

    rx_state_t st = { .cfg = cfg, .sink = sink, .ctx = sink_ctx };
    size_t held_len = 0;
    bool have_held = false;
    uint8_t expected = 1;
    int errors = 0;
    size_t noise = 0;
    esp_err_t result = ESP_OK;
    uint8_t c = 0;

    if (io->purge) {
        io->purge(io->ctx);
    }

    /* Ask for CRC mode, once a second, until the sender starts. */
    bool started = false;
    const int64_t give_up = now_ms() + start_timeout;
    while (!started && now_ms() < give_up) {
        send_byte(io, CRC_REQUEST);
        const int64_t next_poll = now_ms() + POLL_MS;
        while (read_byte_by(io, &c, next_poll)) {
            if (c == SOH || c == STX || c == EOT || c == CAN) {
                started = true;
                break;
            }
        }
    }
    if (!started) {
        result = ESP_ERR_TIMEOUT;
        goto cancel;
    }

    for (;;) {
        if (c == EOT) {
            if (have_held) {
                result = deliver(&st, held, held_len, true);
                if (result != ESP_OK) {
                    goto cancel;
                }
            }
            send_byte(io, ACK);
            /* A sender whose ACK was lost repeats EOT; stay long enough to answer it again,
             * or it gives up on a transfer that in fact arrived whole. */
            const int64_t linger = now_ms() + EOT_LINGER_MS;
            while (read_byte_by(io, &c, linger)) {
                if (c == EOT) {
                    send_byte(io, ACK);
                }
            }
            if (cfg->expected_size && st.delivered < cfg->expected_size) {
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        if (c == CAN) {
            if (read_byte(io, &c, POLL_MS)) {
                if (c == CAN) {
                    result = ESP_ERR_INVALID_STATE;
                    goto done;
                }
                continue;   /* a lone CAN is line noise; c holds the next byte */
            }
            c = 0;          /* then silence: wait for a header below */
        }

        if (c == SOH || c == STX) {
            const size_t n = (c == STX) ? 1024 : 128;
            size_t got = 0;
            const bool whole = read_exact(io, frame, 2 + n + 2, BLOCK_TIMEOUT_MS, &got);
            bool good = whole && frame[0] == (uint8_t)~frame[1];
            uint16_t crc_rx = 0, crc_calc = 0;
            if (good) {
                crc_rx = (uint16_t)((frame[2 + n] << 8) | frame[3 + n]);
                crc_calc = xmodem_crc16(frame + 2, n);
                good = crc_rx == crc_calc;
            }
            if (!good) {
                if (whole) {
                    s->bad_blocks++;
                } else {
                    s->timeouts++;
                }
                if (s->bad_blocks + s->timeouts == 1) {
                    s->diag_len = got;
                    memcpy(s->diag_head, frame, got < sizeof(s->diag_head) ? got : sizeof(s->diag_head));
                    s->diag_crc_rx = crc_rx;
                    s->diag_crc_calc = crc_calc;
                }
            }
            if (good) {
                const uint8_t num = frame[0];
                if (num == expected) {
                    /* ACK first, then hand over the block before this one while the next is
                     * already on its way: the UART keeps receiving through a flash write (its
                     * ISR is in IRAM). Handing over first delayed the ACK by a flash write's
                     * ~1.7 ms, and at 460800 baud macOS's CH34x driver then dropped the whole
                     * of the sender's next block every few blocks -- 10 s lost to each. The
                     * same 1.7 ms as a busy-wait, with no flash write, did the same, so it is
                     * the delay. A sink that fails now cancels the transfer a block later. */
                    send_byte(io, ACK);
                    if (have_held) {
                        result = deliver(&st, held, held_len, false);
                        if (result != ESP_OK) {
                            goto cancel;
                        }
                    }
                    memcpy(held, frame + 2, n);
                    held_len = n;
                    have_held = true;
                    expected++;
                    s->blocks++;
                    errors = 0;
                    noise = 0;
                } else if (num == (uint8_t)(expected - 1)) {
                    send_byte(io, ACK);   /* a resend: our ACK was lost */
                } else {
                    result = ESP_ERR_INVALID_RESPONSE;
                    goto cancel;
                }
            } else {
                if (++errors > MAX_ERRORS) {
                    result = ESP_FAIL;
                    goto cancel;
                }
                s->retries++;
                wait_quiet(io);
                send_byte(io, NAK);
            }
        } else if (c != 0) {
            /* Anything else where a header belongs is noise, and skipped -- up to a point. */
            s->skipped++;
            if (++noise > MAX_NOISE) {
                result = ESP_FAIL;
                goto cancel;
            }
        }

        if (!read_byte(io, &c, HEADER_TIMEOUT_MS)) {
            if (++errors > MAX_ERRORS) {
                result = ESP_ERR_TIMEOUT;
                goto cancel;
            }
            s->retries++;
            send_byte(io, NAK);
            if (!read_byte(io, &c, HEADER_TIMEOUT_MS)) {
                result = ESP_ERR_TIMEOUT;
                goto cancel;
            }
        }
    }
    goto done;

cancel:
    send_cancel(io);
done:
    s->bytes = st.delivered;
    free(frame);
    free(held);
    return result;
}

/* ------------------------------------------------------------------ send */

/* The next ACK, NAK or CAN; anything else (a stray 'C' queued before the first block,
 * say) is skipped. 0 on timeout. */
static uint8_t wait_reply(const xmodem_io_t *io, uint32_t timeout_ms)
{
    const int64_t deadline = now_ms() + timeout_ms;
    uint8_t c;
    while (read_byte_by(io, &c, deadline)) {
        if (c == ACK || c == NAK || c == CAN) {
            return c;
        }
    }
    return 0;
}

/* Fill `buf` from the source until `len` bytes or the end. */
static int fill(xmodem_source_t source, void *ctx, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const int n = source(ctx, buf + got, len - got);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return (int)got;
}

esp_err_t xmodem_send(const xmodem_io_t *io, const xmodem_config_t *config,
                      xmodem_source_t source, void *source_ctx, xmodem_stats_t *stats)
{
    const xmodem_config_t defaults = { 0 };
    const xmodem_config_t *cfg = config ? config : &defaults;
    const uint32_t start_timeout = cfg->start_timeout_ms ? cfg->start_timeout_ms : START_TIMEOUT_MS;
    xmodem_stats_t local = { 0 };
    xmodem_stats_t *s = stats ? stats : &local;
    memset(s, 0, sizeof(*s));

    /* header + 2 block-number bytes + data + up to 2 check bytes */
    uint8_t *frame = malloc(3 + MAX_BLOCK + 2);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;

    /* 'C' asks for CRC (and we then use 1K blocks); NAK asks for the checksum original. */
    bool crc = false;
    bool started = false;
    uint8_t c;
    const int64_t give_up = now_ms() + start_timeout;
    while (!started && read_byte_by(io, &c, give_up)) {
        if (c == CRC_REQUEST || c == NAK) {
            crc = (c == CRC_REQUEST);
            started = true;
        } else if (c == CAN && read_byte(io, &c, POLL_MS) && c == CAN) {
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
    }
    if (!started) {
        result = ESP_ERR_TIMEOUT;
        goto cancel;
    }

    const size_t block = (crc && cfg->block_size != 128) ? 1024 : 128;
    uint8_t num = 1;
    for (;;) {
        const int n = fill(source, source_ctx, frame + 3, block);
        if (n < 0) {
            result = ESP_FAIL;
            goto cancel;
        }
        if (n == 0) {
            break;
        }
        /* A short tail goes in a 128-byte block, which every 1K receiver also takes. */
        const size_t size = (crc && (size_t)n <= 128) ? 128 : block;
        memset(frame + 3 + n, SUB, size - (size_t)n);
        frame[0] = (size == 1024) ? STX : SOH;
        frame[1] = num;
        frame[2] = (uint8_t)~num;
        size_t frame_len = 3 + size;
        if (crc) {
            const uint16_t sum = xmodem_crc16(frame + 3, size);
            frame[frame_len++] = (uint8_t)(sum >> 8);
            frame[frame_len++] = (uint8_t)sum;
        } else {
            uint8_t sum = 0;
            for (size_t i = 0; i < size; i++) {
                sum += frame[3 + i];
            }
            frame[frame_len++] = sum;
        }

        bool acked = false;
        for (int attempt = 0; attempt <= MAX_ERRORS && !acked; attempt++) {
            if (attempt > 0) {
                s->retries++;
            }
            io->write(io->ctx, frame, frame_len);
            const uint8_t reply = wait_reply(io, REPLY_TIMEOUT_MS);
            if (reply == ACK) {
                acked = true;
            } else if (reply == CAN && read_byte(io, &c, POLL_MS) && c == CAN) {
                result = ESP_ERR_INVALID_STATE;
                goto done;
            }
        }
        if (!acked) {
            result = ESP_FAIL;
            goto cancel;
        }
        s->bytes += (size_t)n;
        s->blocks++;
        num++;
    }

    for (int attempt = 0; attempt <= MAX_ERRORS; attempt++) {
        send_byte(io, EOT);
        const uint8_t reply = wait_reply(io, REPLY_TIMEOUT_MS);
        if (reply == ACK) {
            goto done;
        }
        if (reply == CAN) {
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
    }
    result = ESP_ERR_TIMEOUT;

cancel:
    send_cancel(io);
done:
    free(frame);
    return result;
}
