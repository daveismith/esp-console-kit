/*
 * canbus: the TWAI controller through IDF's node driver (esp_twai_onchip.h). See canbus.h.
 *
 * The ISR's side: the driver's interrupt runs our four callbacks. With CONFIG_TWAI_ISR_CACHE_SAFE
 * (the application's sdkconfig.defaults) it keeps running while a flash write has the cache off,
 * so a frame that arrives during an OTA write, an NVS commit or the console's history save still
 * reaches the receive queue rather than overrunning the controller's 64-byte FIFO. That is why
 * everything the callbacks touch is in IRAM or internal RAM: the callbacks, the receive queue and
 * its storage, and the send pool. The queues and the pool are allocated at the first start, so a
 * board that never uses CAN gives them no RAM, and are kept from then on.
 *
 * Sending: the driver queues pointers, not copies, so a frame must stay put until the ISR says it
 * is done. The pool holds CONFIG_CANBUS_TX_QUEUE_LEN of them; a free-list queue hands them out
 * and on_tx_done hands them back. Each start puts config.tx_slots of them in the free list (all,
 * by default), which is how a user limits the frames waiting inside canbus. The driver's own
 * queue is as deep as the pool, so it never refuses a frame for room, and every frame has the
 * same priority, so they leave in the order given. Every frame given to the driver ends counted
 * once, as sent or as failed.
 *
 * Targets (IDF 6.1, read 2026-11-21): the ESP32-S3 and the ESP32-P4 have the same SJA1000-style
 * controller and both run twai_hal_v1.c; the P4 has three of them, and the driver takes the first
 * free one. The bit timing differs only in its clock (canbus.h, canbus_config_t.bitrate). The S3's listen-only REC of 128 (TWAI_LL_HAS_LOM_DOM_ISSUE) is not the P4's: its
 * REC reads 0 there.
 *
 * Bus-off (IDF 6.1's esp_twai_onchip.c and twai_hal_v1.c, read 2026-11-19). The controller has
 * one TX buffer. Bus-off is reached only by transmit errors, so a frame is in that buffer when it
 * comes; the controller drops into reset mode and aborts it. The driver reports the state change
 * first and then a TX-done only if the same interrupt also carried the transmit interrupt, which
 * the controller need not raise for an abort (neither target has the lost-interrupt errata path,
 * TWAI_LL_HAS_INTR_LOST_ISSUE, that would make one up). Frames still in the driver's queue are
 * kept and start going out when the recovery completes; at that moment the driver overwrites, or
 * clears, its pointer to the aborted frame without a callback. So: at bus-off we note that the
 * oldest frame given (the one in the buffer) is owed a TX-done; a TX-done during bus-off pays it;
 * if the recovery completes still owing, that frame is counted failed and its slot freed, in
 * on_state_change, which the driver calls before it refills the buffer. A slot the driver still
 * held when the node was deleted is reclaimed by canbus_stop(), and counted failed too.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canbus.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "canbus";

/* CAN's error levels, on the larger of the two counters (canbus_state_t) */
#define CANBUS_WARNING_LEVEL    96      /* the error warning limit IDF's HAL gives the controller (TWAI_HAL_INIT_EWL) */
#define CANBUS_PASSIVE_LEVEL    128

typedef struct {
    twai_frame_t frame;     /* first: the driver hands this pointer back in on_tx_done */
    uint8_t data[8];
    volatile uint32_t seq;  /* the order it was given to the driver in: the oldest leaves first */
    volatile bool inflight; /* given to the driver, not yet done */
} slot_t;

static SemaphoreHandle_t s_lock;    /* start, stop, send, recover and status against each other */
static twai_node_handle_t s_node;
static canbus_config_t s_config;
static canbus_handle_t s_handle;    /* the running one's, or the last run's once stopped; never 0 after a start */
static QueueHandle_t s_rxq;         /* canbus_frame_t, filled by on_rx_done; never deleted */
static QueueHandle_t s_free;        /* slot_t *, free send slots; never deleted */
static slot_t *s_slots;             /* CONFIG_CANBUS_TX_QUEUE_LEN of them; never freed */
static uint32_t s_seq;              /* the next slot_t.seq; under s_lock */
static int s_tx_slots;              /* slots this run hands out: config.tx_slots, or all */
static volatile bool s_owed;        /* bus-off aborted the frame in the TX buffer, and no TX-done yet */

_Static_assert(sizeof(canbus_frame_t) == 24, "canbus_frame_t is the receive queue's item: keep it 24 bytes");

/* canbus_wake()'s item in the receive queue: no frame has this length, and canbus_receive() skips it */
#define WAKE_LEN 0xFF

/* Counters since canbus_start(): written by the ISR, read whole (32-bit) by anyone */
static volatile uint32_t s_tx_frames, s_tx_failed, s_rx_frames, s_rx_dropped, s_bus_errors, s_bus_off;
static volatile uint32_t s_rx_peak;     /* the most frames the receive queue has held */

/* ------------------------------------------------------------------ ISR */

static IRAM_ATTR bool on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    (void)edata;
    (void)ctx;
    /* The driver writes the payload straight into the frame queued (none for a remote frame) */
    canbus_frame_t out = { .timestamp_us = (uint64_t)esp_timer_get_time() };
    twai_frame_t f = { .buffer = out.data, .buffer_len = sizeof(out.data) };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) {
        return false;
    }
    out.id = f.header.id;
    out.ext = f.header.ide;
    out.rtr = f.header.rtr;
    const uint16_t len = twaifd_dlc2len(f.header.dlc);
    out.len = len > 8 ? 8 : (uint8_t)len;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_rxq, &out, &woken) == pdTRUE) {
        s_rx_frames++;
        const uint32_t depth = uxQueueMessagesWaitingFromISR(s_rxq);
        if (depth > s_rx_peak) {
            s_rx_peak = depth;
        }
    } else {
        s_rx_dropped++;
    }
    return woken == pdTRUE;
}

/* The frame given longest ago and not done: the one in the TX buffer, or next into it */
static IRAM_ATTR slot_t *oldest_inflight(void)
{
    slot_t *oldest = NULL;
    for (int i = 0; i < CONFIG_CANBUS_TX_QUEUE_LEN; i++) {
        slot_t *slot = &s_slots[i];
        if (slot->inflight && (oldest == NULL || (int32_t)(slot->seq - oldest->seq) < 0)) {
            oldest = slot;
        }
    }
    return oldest;
}

static IRAM_ATTR bool release_from_isr(slot_t *slot, bool sent)
{
    if (sent) {
        s_tx_frames++;
    } else {
        s_tx_failed++;
    }
    slot->inflight = false;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_free, &slot, &woken);
    return woken == pdTRUE;
}

static IRAM_ATTR bool on_tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *ctx)
{
    (void)node;
    (void)ctx;
    s_owed = false;     /* during bus-off, the only frame that can be done is the aborted one */
    return release_from_isr((slot_t *)edata->done_tx_frame, edata->is_tx_success);
}

static IRAM_ATTR bool on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t *edata, void *ctx)
{
    (void)node;
    (void)ctx;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF && edata->old_sta != TWAI_ERROR_BUS_OFF) {
        s_bus_off++;
        s_owed = oldest_inflight() != NULL;
    } else if (edata->old_sta == TWAI_ERROR_BUS_OFF && edata->new_sta != TWAI_ERROR_BUS_OFF) {
        /* Recovered: the driver is about to refill the TX buffer from its queue, forgetting the
         * frame bus-off aborted if it never reported it */
        slot_t *lost = s_owed ? oldest_inflight() : NULL;
        s_owed = false;
        if (lost != NULL) {
            return release_from_isr(lost, false);
        }
    }
    return false;
}

static IRAM_ATTR bool on_error(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    (void)node;
    (void)ctx;
    /* The driver calls this for a bus error or a lost arbitration. Losing arbitration is how CAN
     * works, not an error, so a call with that flag alone isn't counted. Every other is, flags or
     * none: IDF 6.1's HAL (twai_hal_v1.c, twai_hal_get_events()) sets a flag only for a bit, form
     * or stuff error, or an "other" error in the ACK slot, so a CRC error, say, comes with none. */
    const twai_error_flags_t f = edata->err_flags;
    const twai_error_flags_t arb_lost_alone = { .arb_lost = 1 };
    if (f.val != arb_lost_alone.val) {
        s_bus_errors++;
    }
    return false;
}

/* ------------------------------------------------------------------ API */

esp_err_t canbus_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The queues and the send pool, at the first start: internal RAM (FreeRTOS's own allocations
 * are), as the cache-safe ISR needs. They are kept from then on. */
static esp_err_t make_queues(void)
{
    if (s_rxq == NULL) {
        s_rxq = xQueueCreate(CONFIG_CANBUS_RX_QUEUE_LEN, sizeof(canbus_frame_t));
    }
    if (s_slots == NULL) {
        s_slots = heap_caps_calloc(CONFIG_CANBUS_TX_QUEUE_LEN, sizeof(slot_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_free == NULL && s_slots != NULL) {
        s_free = xQueueCreate(CONFIG_CANBUS_TX_QUEUE_LEN, sizeof(slot_t *));    /* filled by each start */
    }
    return s_rxq && s_free ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The free list with this run's slots. None is in flight: the last stop took them all back, and
 * nobody sends while stopped. */
static void fill_free(int slots)
{
    s_tx_slots = slots > 0 && slots < CONFIG_CANBUS_TX_QUEUE_LEN ? slots : CONFIG_CANBUS_TX_QUEUE_LEN;
    xQueueReset(s_free);
    for (int i = 0; i < s_tx_slots; i++) {
        slot_t *slot = &s_slots[i];
        xQueueSend(s_free, &slot, 0);
    }
}

/* Whether the controller is bus-off, as the driver has it (it refuses to send while it is, and to
 * recover while it isn't, with an error line each time). Under s_lock, with a node. */
static bool bus_off_locked(void)
{
    twai_node_status_t st;
    return twai_node_get_info(s_node, &st, NULL) == ESP_OK && st.state == TWAI_ERROR_BUS_OFF;
}

/* The handle is the run in progress's: read without the lock by send and receive, whose callers
 * are the owner's own tasks, which end before the owner stops */
static bool owns(canbus_handle_t handle)
{
    return handle != 0 && s_node != NULL && handle == s_handle;
}

esp_err_t canbus_start(const canbus_config_t *config, canbus_handle_t *handle)
{
    *handle = 0;
    ESP_RETURN_ON_ERROR(canbus_init(), TAG, "no memory");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_node ? ESP_ERR_INVALID_STATE : make_queues();
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    canbus_config_t c = *config;
    if (c.bitrate == 0) {
        c.bitrate = CANBUS_DEFAULT_BITRATE;
    }
    const twai_onchip_node_config_t nc = {
        .io_cfg = {
            .tx = c.tx_gpio,
            .rx = c.rx_gpio,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = { .bitrate = c.bitrate, .sp_permill = 875 },
        /* CAN's own rule: retransmit until acknowledged or bus-off. For every frame: anything else
         * (single-shot, on this controller) is the whole node's, fixed until it is deleted; the
         * driver has nothing per frame, which Cyphal's anonymous frames would want (cyphal.c,
         * request()) */
        .fail_retry_cnt = -1,
        .tx_queue_depth = CONFIG_CANBUS_TX_QUEUE_LEN,
        .flags = {
            .enable_self_test = c.mode == CANBUS_SELFTEST,
            .enable_loopback = c.mode == CANBUS_SELFTEST,
            .enable_listen_only = c.mode == CANBUS_LISTEN,
        },
    };
    static const twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
    };
    /* Whatever `gpio set` (or anything else) left on the pins goes: a pin still driven as a
     * GPIO output would fight the transceiver on RX. The driver then routes both. */
    if (c.tx_gpio >= 0) {
        gpio_reset_pin(c.tx_gpio);
    }
    gpio_reset_pin(c.rx_gpio);
    twai_node_handle_t node = NULL;
    err = twai_new_node_onchip(&nc, &node);
    if (err == ESP_OK) {
        err = twai_node_register_event_callbacks(node, &cbs, NULL);
    }
    if (err == ESP_OK) {
        xQueueReset(s_rxq);
        fill_free(c.tx_slots);
        s_tx_frames = s_tx_failed = s_rx_frames = s_rx_dropped = s_bus_errors = s_bus_off = s_rx_peak = 0;
        s_owed = false;
        err = twai_node_enable(node);
    }
    if (err == ESP_OK) {
        s_handle = s_handle + 1 != 0 ? s_handle + 1 : 1;
        s_config = c;
        s_node = node;
        *handle = s_handle;
    } else if (node != NULL) {
        twai_node_delete(node);
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* Disable the node (unless bus-off, which the driver already counts as disabled) and delete it.
 * The state can change between the read and the delete: a bus-off recovery completing makes
 * the delete refuse, a new bus-off makes the disable refuse. A disable that succeeds turns the
 * interrupt off, so the state stays put and the second round's delete goes through. */
static esp_err_t delete_node(void)
{
    esp_err_t err = ESP_FAIL;
    for (int round = 0; round < 3 && err != ESP_OK; round++) {
        twai_node_status_t st;
        if (twai_node_get_info(s_node, &st, NULL) == ESP_OK && st.state != TWAI_ERROR_BUS_OFF) {
            twai_node_disable(s_node);
        }
        err = twai_node_delete(s_node);
    }
    return err;
}

/* A pin given back: a floating input, as the `gpio` command's `release` leaves one */
static void float_pin(int pin)
{
    if (pin >= 0) {
        gpio_reset_pin(pin);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
}

esp_err_t canbus_stop(canbus_handle_t handle)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (owns(handle)) {
        err = delete_node();
        if (err == ESP_OK) {
            s_node = NULL;
            /* The ISR is gone: what the driver still held comes back to the pool, unsent */
            for (int i = 0; i < CONFIG_CANBUS_TX_QUEUE_LEN; i++) {
                if (s_slots[i].inflight) {
                    s_slots[i].inflight = false;
                    s_tx_failed++;
                    slot_t *slot = &s_slots[i];
                    xQueueSend(s_free, &slot, 0);
                }
            }
            float_pin(s_config.tx_gpio);
            float_pin(s_config.rx_gpio);
        } else {
            ESP_LOGE(TAG, "the node would not stop: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t canbus_send(canbus_handle_t handle, const canbus_frame_t *frame, TickType_t wait)
{
    if (frame->len > 8 || frame->id > (frame->ext ? CANBUS_EXT_ID_MAX : CANBUS_STD_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!owns(handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.mode == CANBUS_LISTEN) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* A slot first, outside the lock: a stop meanwhile must not wait for room on a dead bus */
    slot_t *slot;
    if (xQueueReceive(s_free, &slot, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    /* Bus-off refused here rather than by the driver, which would log it: the node could ask every
     * tick. Bus-off can still begin between this and the hand-over; then the driver refuses. */
    if (owns(handle) && !bus_off_locked()) {
        memcpy(slot->data, frame->data, frame->len);
        slot->frame = (twai_frame_t){
            .header = {
                .id = frame->id,
                .dlc = frame->len,
                .ide = frame->ext,
                .rtr = frame->rtr,
            },
            .buffer = slot->data,
            .buffer_len = frame->rtr ? 0 : frame->len,
        };
        slot->seq = s_seq++;    /* before inflight: the ISR orders by it once inflight is seen */
        slot->inflight = true;
        err = twai_node_transmit(s_node, &slot->frame, 0);
    }
    if (err != ESP_OK) {
        slot->inflight = false;
        xQueueSend(s_free, &slot, 0);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t canbus_wait_room(canbus_handle_t handle, TickType_t wait)
{
    if (!owns(handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    slot_t *slot;
    return xQueuePeek(s_free, &slot, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool canbus_receive(canbus_handle_t handle, canbus_frame_t *frame, TickType_t wait)
{
    /* A running handle means the queue exists: it is made at the first start */
    if (!owns(handle)) {
        return false;
    }
    while (xQueueReceive(s_rxq, frame, wait) == pdTRUE) {
        if (frame->len != WAKE_LEN) {
            return true;
        }
        /* canbus_wake()'s: whatever else is queued, without waiting. So a false return still means
         * the queue was empty, which the Cyphal node's listen relies on (cyphal_listen_empty()) */
        wait = 0;
    }
    return false;
}

esp_err_t canbus_wake(canbus_handle_t handle)
{
    if (!owns(handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Behind any frames already queued; a stale one left by a stop is cleared by the next start */
    const canbus_frame_t wake = { .len = WAKE_LEN };
    return xQueueSend(s_rxq, &wake, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t canbus_recover(canbus_handle_t handle)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Not bus-off refused here, without the driver's error line */
    const esp_err_t err = owns(handle) && bus_off_locked() ? twai_node_recover(s_node) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_lock);
    return err;
}

bool canbus_running(void)
{
    return s_node != NULL;
}

const char *canbus_owner(void)
{
    return s_node != NULL ? s_config.owner : NULL;
}

void canbus_status(canbus_status_t *out)
{
    *out = (canbus_status_t){ .state = CANBUS_STOPPED };
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->config = s_config;
    if (s_node != NULL) {
        twai_node_status_t st;
        if (twai_node_get_info(s_node, &st, NULL) == ESP_OK) {
            /* Bus-off as the driver has it. The rest from the counters just read: the driver's state
             * changes on the way up (warning, passive) and at a recovery, but not when the counters
             * fall back below the warning level, so it could contradict the counters beside it */
            const unsigned worst = st.tx_error_count > st.rx_error_count ? st.tx_error_count : st.rx_error_count;
            out->state = st.state == TWAI_ERROR_BUS_OFF ? CANBUS_BUS_OFF
                         : worst >= CANBUS_PASSIVE_LEVEL ? CANBUS_ERROR_PASSIVE
                         : worst >= CANBUS_WARNING_LEVEL ? CANBUS_ERROR_WARNING : CANBUS_ERROR_ACTIVE;
            out->tx_errors = st.tx_error_count;
            out->rx_errors = st.rx_error_count;
        }
    }
    out->tx_frames = s_tx_frames;
    out->tx_failed = s_tx_failed;
    out->rx_frames = s_rx_frames;
    out->rx_dropped = s_rx_dropped;
    out->rx_queue_peak = s_rx_peak;
    out->bus_errors = s_bus_errors;
    out->bus_off = s_bus_off;
    out->tx_pending = s_free ? s_tx_slots - uxQueueMessagesWaiting(s_free) : 0;
    xSemaphoreGive(s_lock);
}

const char *canbus_format_bitrate(char *buf, size_t len, uint32_t bitrate)
{
    if (bitrate % 1000000 == 0) {
        snprintf(buf, len, "%lu Mbit/s", (unsigned long)(bitrate / 1000000));
    } else if (bitrate % 1000 == 0) {
        snprintf(buf, len, "%lu kbit/s", (unsigned long)(bitrate / 1000));
    } else {
        snprintf(buf, len, "%lu bit/s", (unsigned long)bitrate);
    }
    return buf;
}

bool canbus_parse_bitrate(const char *s, uint32_t *bitrate)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || !isdigit((unsigned char)*s)) {
        return false;
    }
    unsigned long unit = 1;
    if (*end == 'k' || *end == 'K') {
        unit = 1000;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        unit = 1000000;
        end++;
    }
    /* Checked before multiplying, so that a large number can't wrap into range */
    if (*end != '\0' || v > 1000000 / unit || v * unit < 10000) {
        return false;
    }
    *bitrate = (uint32_t)(v * unit);
    return true;
}

const char *canbus_state_name(canbus_state_t state)
{
    switch (state) {
    case CANBUS_ERROR_ACTIVE: return "error-active";
    case CANBUS_ERROR_WARNING: return "error-warning";
    case CANBUS_ERROR_PASSIVE: return "error-passive";
    case CANBUS_BUS_OFF: return "bus-off";
    default: return "stopped";
    }
}
