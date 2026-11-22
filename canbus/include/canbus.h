/*
 * canbus -- one TWAI (CAN) controller, classic frames only: started on two pins at a bitrate,
 * frames in through a queue the ISR fills, frames out through a pool the ISR empties, and the
 * controller's error state. One owner at a time: the `can` bench command (cmd_can.c) or a Cyphal
 * node. canbus_start() gives the owner a handle, and only a call with it stops, sends, reads or
 * recovers; the others can look (canbus_owner(), canbus_status()). The pins are the caller's to
 * choose and to guard.
 *
 * Targets: the ESP32-S3 (one controller) and the ESP32-P4 (three; IDF's driver gives this run
 * the first that is free, TWAI0 unless another driver has it). canbus runs one node at a time.
 *
 * Every frame the bus carries is accepted (the controller's one filter is left open), so a
 * protocol on top filters in software.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANBUS_DEFAULT_BITRATE 1000000
#define CANBUS_STD_ID_MAX 0x7FFu        /* an 11-bit ID */
#define CANBUS_EXT_ID_MAX 0x1FFFFFFFu   /* a 29-bit ID */

typedef enum {
    CANBUS_NORMAL,      /* sends, receives, acknowledges */
    CANBUS_LISTEN,      /* receives only: never drives the bus, not even an acknowledgement */
    CANBUS_SELFTEST,    /* receives its own frames back through the transceiver, needs no ACK */
} canbus_mode_t;

typedef struct {
    int tx_gpio;
    int rx_gpio;
    /* bits/s; 0 for CANBUS_DEFAULT_BITRATE. 87.5 % is asked for as the sample point; IDF 6.1's
     * timing calculation makes it 85 % at 125k, 250k, 500k and 1M on both targets: 20 quanta
     * (sync 1, TSEG1 16, TSEG2 3; SJW 1), since TSEG1 is 16 at most and the prescaler even, which
     * rules out 87.5 % at 1 Mbit/s. The quanta come from the APB clock (80 MHz, prescaler 4 at
     * 1 Mbit/s) on the S3 and from the crystal (40 MHz, prescaler 2) on the P4 */
    uint32_t bitrate;
    canbus_mode_t mode;
    /* Who holds the controller, as a phrase for another user's refusal ("the Cyphal node"):
     * wording only, kept as a pointer, so a static string. The handle is what canbus checks. */
    const char *owner;
    /* Frames canbus_send() lets wait inside canbus at once, 1 to CONFIG_CANBUS_TX_QUEUE_LEN; 0 for
     * all of them. They leave in the order given and can't be taken back, so a protocol that
     * orders its frames by priority and gives them deadlines (Cyphal) keeps this at 1 and its
     * own queue in front: then one frame at most goes out late or out of order. */
    int tx_slots;
} canbus_config_t;

/* In this order so that it packs into 24 bytes, the receive queue's item (checked in canbus.c) */
typedef struct {
    uint64_t timestamp_us;  /* esp_timer_get_time() in the ISR that took it off the controller */
    uint32_t id;            /* 11 bits, or 29 when `ext` */
    uint8_t len;            /* 0..8; a remote frame's requested length */
    bool ext;
    bool rtr;
    uint8_t data[8];
} canbus_frame_t;

/* The run canbus_start() began, for the calls only its owner may make; 0 is none. A handle from
 * an earlier run is refused, as another owner's is. */
typedef uint32_t canbus_handle_t;

typedef enum {
    CANBUS_STOPPED,
    CANBUS_ERROR_ACTIVE,
    CANBUS_ERROR_WARNING,   /* an error counter at 96 or more */
    CANBUS_ERROR_PASSIVE,   /* an error counter at 128 or more */
    CANBUS_BUS_OFF,         /* off the bus until canbus_recover() */
} canbus_state_t;

typedef struct {
    canbus_state_t state;
    canbus_config_t config;     /* as started */
    uint16_t tx_errors;         /* the controller's TEC */
    uint16_t rx_errors;         /* REC */
    /* Since canbus_start(): */
    uint32_t tx_frames;         /* acknowledged (or, in self-test, sent) */
    uint32_t tx_failed;         /* not sent: given up on, aborted by bus-off, or dropped by a stop */
    uint32_t rx_frames;         /* put on the receive queue */
    uint32_t rx_dropped;        /* the receive queue was full */
    uint32_t rx_queue_peak;     /* the most frames the receive queue has held (of CONFIG_CANBUS_RX_QUEUE_LEN) */
    uint32_t bus_errors;        /* bit, form, stuff and ACK errors seen; lost arbitration is not one */
    uint32_t bus_off;           /* times the controller went bus-off */
    uint32_t tx_pending;        /* frames given to canbus_send() and not yet done */
} canbus_status_t;

/* The lock the calls below share. Once, from app_main, before another task can call them; again
 * is harmless. canbus_start() calls it too, for a caller with nothing to register. */
esp_err_t canbus_init(void);

/* Take the controller and the two pins, and start; *handle is the run's, for the calls below
 * (0 on failure). The pins are reset first, whatever was driving them as GPIOs: the caller has
 * checked that nothing else owns them. ESP_ERR_INVALID_STATE if already started,
 * ESP_ERR_NOT_FOUND if another driver holds the controller, ESP_ERR_INVALID_ARG for pins or a
 * bitrate the controller can't do. Frames queued from an earlier run are discarded. */
esp_err_t canbus_start(const canbus_config_t *config, canbus_handle_t *handle);

/* The calls below are the owner's: with any handle but the running one's they do nothing and
 * return ESP_ERR_INVALID_STATE (canbus_receive(), false). */

/* Stop, and leave the pins floating inputs. Frames not yet sent are dropped (counted in
 * tx_failed). If the driver will not let the node go, it is left running (the handle still
 * good) and the error returned. */
esp_err_t canbus_stop(canbus_handle_t handle);

/* Queue one frame for sending, waiting up to `wait` for room (config.tx_slots frames are in
 * flight at most; the wait ends as soon as one is done). ESP_ERR_TIMEOUT when there was none,
 * ESP_ERR_NOT_SUPPORTED in listen-only, ESP_ERR_INVALID_STATE when stopped or bus-off. Frames go
 * out in the order given. */
esp_err_t canbus_send(canbus_handle_t handle, const canbus_frame_t *frame, TickType_t wait);

/* Wait up to `wait` for room to send one frame, without taking it: ESP_OK when canbus_send()
 * with no wait will find room (for a single sending task), ESP_ERR_TIMEOUT when there was none.
 * For a sender that must look at something between the wait and the frame, as the Cyphal node
 * reads the frames that came meanwhile. */
esp_err_t canbus_wait_room(canbus_handle_t handle, TickType_t wait);

/* The next received frame, waiting up to `wait`. False on timeout, or when canbus_wake() cut the wait
 * short with no frame queued: false always means the queue was found empty. One consumer at a time:
 * the owner, from after its canbus_start() until before its canbus_stop() (as cmd_can.c does). */
bool canbus_receive(canbus_handle_t handle, canbus_frame_t *frame, TickType_t wait);

/* Wake the owner's reader: its canbus_receive() that is waiting, or else its next one, returns at
 * once (with a frame if one is queued). For another task with work for a reader that sleeps in
 * canbus_receive(), as the Cyphal node's worker has, or one that stops it. From any task, with the run's handle:
 * ESP_ERR_INVALID_STATE with any other; ESP_FAIL when the receive queue is full, which leaves the
 * reader frames to read and so awake anyway. */
esp_err_t canbus_wake(canbus_handle_t handle);

/* Start the controller's way back from bus-off: it rejoins after 128 x 11 recessive bits.
 * ESP_ERR_INVALID_STATE if it isn't bus-off. The frame bus-off aborted is lost (counted in
 * tx_failed); the frames waiting behind it go out once the controller is back. */
esp_err_t canbus_recover(canbus_handle_t handle);

/* State, error counters and frame counts. Callable from any task, started or not. */
void canbus_status(canbus_status_t *out);

bool canbus_running(void);

/* config.owner of the run in progress, or NULL when stopped: for wording another user's refusal */
const char *canbus_owner(void);

/* A bitrate in words: "1 Mbit/s", "500 kbit/s", "33333 bit/s". Returns buf. */
const char *canbus_format_bitrate(char *buf, size_t len, uint32_t bitrate);

/* A bitrate as typed: digits, with k or m (either case) for thousands or millions: 1000000, 1000k
 * and 1m are the same. False unless it is 10 kbit/s to 1 Mbit/s. */
bool canbus_parse_bitrate(const char *s, uint32_t *bitrate);

const char *canbus_state_name(canbus_state_t state);

#ifdef __cplusplus
}
#endif
