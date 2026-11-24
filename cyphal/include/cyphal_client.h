/*
 * The node as a Cyphal client (CONFIG_CYPHAL_CLIENT): it calls services on other nodes and hands each response to a
 * callback on the node's worker (the kit's README, "cyphal"). What it calls: the application's CYPHAL_PORT_CLI ports,
 * whose service-IDs are registers (`uavcan.cln.<name>.id`) and whose handlers give the response's extent; and the
 * standard services the kit has types for, on the IDs the standard fixes, with no port of the application's
 * (CYPHAL_CALL_FIXED()). Only built with the option on; with it off nothing here exists.
 *
 * The first half is plain C, no ESP-IDF, so that tests/host builds it (cyphal_client.c): the calls' records and
 * states, the transfer-IDs, a response matched to its call, calls timed out and failed. The second half is the
 * node's (cyphal.c): cyphal_call(), cyphal_call_wait(), the status.
 *
 * A call goes through its record's states, each changed by one party at a time, by a compare-and-swap where two
 * may race (the caller and the node's task, over a QUEUED call):
 *   FREE -> FILLING        the caller (cyphal_call_take()), which fills the request in
 *   FILLING -> QUEUED      the caller, done (cyphal_call_queue()): for the node's task to send
 *   QUEUED -> FREE         the caller, if it finds the node no longer online before the node's task took it
 *                          (cyphal_call_cancel()): then no callback, and cyphal_call() says why
 *   QUEUED -> SENDING      the node's task (cyphal_call_begin(), which counts it as made): it gives the call its
 *                          transfer-ID and queues the request, or ends it (timed out before it could go, the node not
 *                          online)
 *   SENDING -> QUEUED      the node's task, no room in the transmit queue for its request yet (cyphal_call_hold(),
 *                          which uncounts it): it keeps its place
 *   SENDING -> SENT        the node's task: the request queued for the bus; the answer awaited
 *   SENDING, SENT -> DONE  the node's task (cyphal_call_finish()): answered, its response copied in; timed out; not
 *                          sent; or failed, the node stopped, restarted or without its node-ID (also a stop, with
 *                          the node's task ended)
 *   DONE -> FREE           the worker, its outcome taken for the callback (cyphal_call_free())
 * So every call taken and queued ends in exactly one callback, or in its caller's cancel. The state word also counts
 * the record's takes (its generation, above the state's bits: CYPHAL_CALL_STATE()), and the two swaps a caller's
 * cancel can race with check it: the cancel and the node task's begin each name the word they saw (a ticket), so a
 * cancel late enough to find the record freed and taken by another call fails, and a begin never takes a record
 * that changed hands since it was looked at. The node's task reads a record's fields only once its begin has
 * succeeded. The pool counts its records not free (`held`, atomic): with none, the node task's and the worker's looks
 * at it return at once without a scan. A response is matched to a SENT call by its service-ID, its source (the
 * call's server) and its transfer-ID; any other is dropped and counted: one that comes after its call timed out, one
 * from another node, one for no call.
 *
 * Order: each call queued is stamped with the pool's next sequence number, and the node's task takes the oldest
 * queued first (cyphal_call_next_queued()), the worker the oldest ended (cyphal_call_done()).
 *
 * Transfer-IDs: one counter for each port the client calls and each server, as Cyphal keeps one for each session
 * (a request's is its service, source and destination), so a server never sees a call's transfer-ID repeat the one
 * before it, whatever else was called meanwhile. A transfer-ID a call still awaiting its answer holds (SENT, the same
 * service and server) is skipped, so no two calls out can be answered by the same response. Kept across the node's
 * restarts: a server still holding the last one doesn't take the next for a repeat.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cyphal_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CYPHAL_CALL_REQUEST_MAX   515   /* a request, serialized: bytes at most (uavcan.register.Access's, the largest
                                         * the kit has a type for) */
#define CYPHAL_CALL_RESPONSE_MAX  448   /* a response, as kept for the callback: bytes at most (uavcan.node.GetInfo's
                                         * extent); a CYPHAL_PORT_CLI handler's extent may not be more */
#define CYPHAL_CALL_SERVERS       128   /* the node-IDs a call can go to: a transfer-ID counter each, per port */

/* A call's outcome, on the node's worker. `err` is an esp_err_t (an int, so this half needs no ESP-IDF): ESP_OK with
 * the response's payload, serialized as the server sent it and cut to the port's extent (its handler's, or the
 * standard type's); ESP_ERR_TIMEOUT, no answer by the call's timeout; ESP_ERR_NO_MEM, the request couldn't be
 * queued (libcanard's heap full); ESP_ERR_INVALID_STATE, the node stopped, restarted or lost its node-ID first.
 * Without a response, payload is NULL and size 0. The call's record is free by then (the payload is a copy, the
 * worker's), so a callback may make one more call even with every other record in use: cyphal_call(), never
 * cyphal_call_wait(). It must not keep the payload */
typedef void (*cyphal_response_cb_t)(int err, const uint8_t *payload, size_t size, void *ctx);

typedef enum {
    CYPHAL_CALL_FREE,
    CYPHAL_CALL_FILLING,
    CYPHAL_CALL_QUEUED,
    CYPHAL_CALL_SENDING,
    CYPHAL_CALL_SENT,
    CYPHAL_CALL_DONE,
} cyphal_call_state_t;

/* A record's state word: its state in the low bits, its generation (how many times it has been taken) above */
#define CYPHAL_CALL_STATE_BITS 3
#define CYPHAL_CALL_STATE(word) ((cyphal_call_state_t)((word) & ((1u << CYPHAL_CALL_STATE_BITS) - 1)))

typedef enum {
    CYPHAL_CALL_ANSWERED,
    CYPHAL_CALL_TIMED_OUT,
    CYPHAL_CALL_NOT_SENT,       /* its request couldn't be queued */
    CYPHAL_CALL_FAILED,         /* the node stopped, restarted or lost its node-ID before an answer */
} cyphal_call_outcome_t;

/* One call */
typedef struct {
    /* The caller's, while FILLING */
    size_t port;                /* the port called: its row in the caller's table of ports (transfer-IDs) */
    uint8_t server;             /* the node called, 0-127 */
    int64_t deadline_us;        /* it times out then, answered or not (esp_timer microseconds on the board) */
    cyphal_response_cb_t cb;    /* NULL for none */
    void *ctx;
    size_t request_size;
    uint8_t request[CYPHAL_CALL_REQUEST_MAX];
    uint32_t seq;               /* its place in the order calls were queued (cyphal_call_queue()): through __atomic */
    /* The node task's, from SENDING */
    uint16_t service_id;        /* as sent: the port's this run */
    uint8_t transfer_id;        /* as sent */
    cyphal_call_outcome_t outcome;  /* DONE: how it ended */
    size_t response_size;
    uint8_t response[CYPHAL_CALL_RESPONSE_MAX];
} cyphal_call_t;

/* The client's counters, since cyphal_call_counts_clear(): calls the node's task took (each ends in a callback), how
 * they ended, and responses that matched no call. So made is answered + timed_out + not_sent + failed and the calls
 * taken still out. Written by the node's task, or a stop with it ended */
typedef struct {
    uint32_t made, answered, timed_out, not_sent, failed, dropped;
} cyphal_call_counts_t;

/* The calls: their records, each one's state and the transfer-IDs, all in the caller's memory (on the board the
 * records and transfer-IDs in PSRAM, the pool and the states in internal RAM for the atomics) */
typedef struct {
    cyphal_call_t *calls;
    uint32_t *states;           /* each record's state word (CYPHAL_CALL_STATE()): through __atomic only */
    size_t count;
    uint32_t held;              /* records not free: through __atomic only */
    uint32_t next_seq;          /* the next call queued's cyphal_call_t.seq: through __atomic only */
    uint8_t *tids;              /* the next transfer-ID for each port and server: ports * CYPHAL_CALL_SERVERS */
    size_t ports;
    cyphal_call_counts_t counts;
} cyphal_call_pool_t;

/* No memory for the records or the states (NULL) is a pool of none: every take fails. Every record free, every
 * transfer-ID 0, the counts 0 */
void cyphal_call_pool_init(cyphal_call_pool_t *p, cyphal_call_t *calls, uint32_t *states, size_t count, uint8_t *tids,
                           size_t ports);
void cyphal_call_counts_clear(cyphal_call_pool_t *p);

/* ---- the caller's: from any task */

/* A free record taken (FILLING, its generation counted on), for the caller to fill in and queue: its index, -1 if
 * none is free */
int cyphal_call_take(cyphal_call_pool_t *p);
/* The filled record, stamped with the next sequence number, for the node's task to send (FILLING -> QUEUED): its
 * ticket, the state word it was queued with, for cyphal_call_cancel() */
uint32_t cyphal_call_queue(cyphal_call_pool_t *p, size_t i);
/* A queued record taken back before the node's task took it (QUEUED -> FREE), if it is still the call `ticket` names:
 * false if the node's task has it already, whose callback will then come (and may have come: the record may since
 * have been freed and taken by another call, which this leaves alone) */
bool cyphal_call_cancel(cyphal_call_pool_t *p, size_t i, uint32_t ticket);

/* ---- the node task's */

/* The oldest call queued (the lowest sequence number): its index and its ticket, the state word seen, for
 * cyphal_call_begin(); -1 for none */
int cyphal_call_next_queued(const cyphal_call_pool_t *p, uint32_t *ticket);
/* Whether a call is queued, waiting for the node's task */
bool cyphal_call_waiting(const cyphal_call_pool_t *p);
/* A queued call taken (QUEUED -> SENDING), counted as made, if it is still the call `ticket` names: false if its
 * caller took it back since (and the record may have another call in it now) */
bool cyphal_call_begin(cyphal_call_pool_t *p, size_t i, uint32_t ticket);
/* A call taken whose request has no room yet given back, queued as it was, its sequence number kept (SENDING ->
 * QUEUED), uncounted as made */
void cyphal_call_hold(cyphal_call_pool_t *p, size_t i);
/* The transfer-ID for the next request to `server` on the port at row `port` of the transfer-IDs, service_id this
 * run, counted on (modulo CANARD_TRANSFER_ID_MAX + 1) past any that a SENT call to that server on service_id holds:
 * with 32 records at most there is always one free */
uint8_t cyphal_call_next_tid(cyphal_call_pool_t *p, size_t port, uint8_t server, uint16_t service_id);
/* The transfer deadline for call i's request queued at now_us: a heartbeat-like `most_us` from now, or just before the
 * call's own deadline if that is sooner, so that no frame of it is handed to the controller once the call has timed
 * out (the node's task drops a transfer whose deadline has passed before its next frame goes) */
int64_t cyphal_call_tx_deadline(const cyphal_call_pool_t *p, size_t i, int64_t now_us, int64_t most_us);
/* Its request queued with that service-ID and transfer-ID (SENDING -> SENT) */
void cyphal_call_sent(cyphal_call_pool_t *p, size_t i, uint16_t service_id, uint8_t transfer_id);
/* A call taken (SENDING or SENT) ended (-> DONE), the outcome counted: for the worker's callback */
void cyphal_call_finish(cyphal_call_pool_t *p, size_t i, cyphal_call_outcome_t outcome);
/* A response (its payload, `size` bytes) on service_id from `source` with transfer_id: the SENT call it answers,
 * with the payload copied in (CYPHAL_CALL_RESPONSE_MAX at most) and DONE, answered; its index. -1, and counted as
 * dropped, for none (its call timed out, or failed; another node's; a transfer-ID nobody waits for) */
int cyphal_call_match(cyphal_call_pool_t *p, uint16_t service_id, uint8_t source, uint8_t transfer_id,
                      const void *payload, size_t size);
/* Every call queued or sent whose deadline is at or before now_us ended, timed out: how many */
size_t cyphal_call_expire(cyphal_call_pool_t *p, int64_t now_us);
/* Every call queued, being sent or sent ended, failed (the node stopped, restarted or lost its node-ID). On the
 * node's task, or with it ended: how many */
size_t cyphal_call_fail_all(cyphal_call_pool_t *p);
/* The earliest deadline of the calls queued or sent: INT64_MAX for none */
int64_t cyphal_call_next_deadline(const cyphal_call_pool_t *p);

/* ---- the worker's */

/* The oldest call DONE (the lowest sequence number), for its callback: its index, -1 for none */
int cyphal_call_done(const cyphal_call_pool_t *p);
/* Its outcome taken (the response copied out): the record free again (DONE -> FREE), before the callback runs */
void cyphal_call_free(cyphal_call_pool_t *p, size_t i);

/* Records not free, of all */
size_t cyphal_call_held(const cyphal_call_pool_t *p);

/* A serialized uavcan.node.port.List (`size` bytes) with service_id added to its clients: the first call of a
 * standard service in a run, which the node subscribes to only then. False, and the list untouched, if it isn't one
 * or service_id isn't a service-ID */
bool cyphal_port_list_add_client(uint8_t *list, size_t size, uint16_t service_id);

/* uavcan.node.Heartbeat.1.0 as another node sends it (cyphal_app_t.on_heartbeat): its uptime is how a client tells
 * that a server restarted, which nothing else a node publishes or answers carries */
typedef struct {
    uint32_t uptime_s;
    uint8_t health;             /* uavcan.node.Health: 0 nominal, 1 advisory, 2 caution, 3 warning */
    uint8_t mode;               /* uavcan.node.Mode: 0 operational, 1 initialization, 2 maintenance, 3 software update */
    uint8_t vssc;               /* the vendor-specific status code */
} cyphal_heartbeat_t;

/* A classic CAN frame (its CAN ID, extended or not, remote or not, and its `len` bytes of data) that is a whole
 * heartbeat from a regular source: one frame, the type's 7 bytes and a tail byte with the start and end of the
 * transfer and the toggle bit set. True, its source into *source and the heartbeat into *out; false, both untouched,
 * for any other frame (anonymous, a service, another subject, a transfer of more than one frame: so a later heartbeat
 * version of more than 7 bytes would not be read). The node reads every heartbeat this way, from the frame alone, as
 * the allocator reads another allocator's answer: no subscription, so no libcanard memory and nothing for each node
 * heard; port.List names 7509 among its subscribers while cyphal_app_t.on_heartbeat is set */
bool cyphal_heartbeat_frame(uint32_t can_id, bool ext, bool rtr, const uint8_t *data, uint8_t len, uint8_t *source,
                            cyphal_heartbeat_t *out);

/* ------------------------------------------------------------------ the node's (cyphal.c) */

/* cyphal_call()'s port for a standard service on the ID the standard fixes, which the node calls with no port of the
 * application's: uavcan.node.GetInfo (430), uavcan.node.ExecuteCommand (435, version 1.3), uavcan.register.Access
 * (384) and List (385), uavcan.file.GetInfo (405) and List (406, both version 0.2). Past every index of the
 * application's port table. The node subscribes to one's responses, and lists it among port.List's clients, from
 * its first call in a run */
#define CYPHAL_CALL_FIXED(service_id) ((size_t)CYPHAL_PORTS_MAX + (size_t)(service_id))
#define CYPHAL_CALL_FIXED_COUNT 6       /* those standard services */

/* The client's counters, since boot (cyphal_init(): not cleared by a stop or a start, so a call a stop failed is
 * counted, and made always adds up), its records now, and the standard services it has called this run (subscribed
 * to, and listed among port.List's clients), in the order above */
typedef struct {
    cyphal_call_counts_t counts;
    size_t held, slots;
    uint16_t fixed[CYPHAL_CALL_FIXED_COUNT];
    size_t fixed_count;
} cyphal_client_status_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"

/*
 * A call to service `port` on node `server`: the request (`size` bytes, serialized, CYPHAL_CALL_REQUEST_MAX at most;
 * none for 0) copied, for the node's task to send at the port's priority (nominal for a standard service), with the
 * next transfer-ID; the outcome to `cb` (cyphal_response_cb_t), on the node's worker, at most timeout_ms after this.
 * `port` is the index in cyphal_app_t.ports of a CYPHAL_PORT_CLI port with a handler, or CYPHAL_CALL_FIXED(). From any
 * task; never waits. Returns, checked in this order:
 *   ESP_OK                 cb will be called, exactly once
 *   ESP_ERR_NOT_SUPPORTED  `port` isn't one of those, or is off this run
 *   ESP_ERR_INVALID_ARG    `server` over 127, a timeout of 0, or a size with no request
 *   ESP_ERR_INVALID_SIZE   a request over CYPHAL_CALL_REQUEST_MAX
 *   ESP_ERR_INVALID_STATE  the node isn't online on a node-ID (also if it stops as the call is queued)
 *   ESP_ERR_INVALID_ARG    `server` is the node itself
 *   ESP_ERR_NOT_FINISHED   every call record in use (CONFIG_CYPHAL_CLIENT_CALLS; none at all if the records couldn't
 *                          be allocated): the caller may try again
 * With any but ESP_OK nothing is sent and cb is never called. cb may run before this returns.
 *
 * Order, as cyphal_publish() keeps its messages': calls are queued for the bus in the order cyphal_call() queued them
 * (a task's, in the order it made them), and a request the transmit queue has no room for yet holds the younger ones
 * behind it. libcanard's queue sends by CAN ID, so requests to one service on one server leave in that order, and
 * those of another priority or to another service or server may pass them. The worker runs the callbacks of the calls
 * ended by its turn oldest call first; a call that ends while an older one is still out has its callback first.
 *
 * Its request is not handed to the controller once the call has timed out: its transfer's deadline is the call's
 * (a second at most, every transfer's), so frames still queued then are dropped. The one frame already inside the
 * controller may still go (on a bus that won't take it: nobody acknowledging, bus-off), which for a single-frame
 * request is all of it.
 */
esp_err_t cyphal_call(size_t port, uint8_t server, const void *request, size_t size, uint32_t timeout_ms,
                      cyphal_response_cb_t cb, void *ctx);

/*
 * cyphal_call(), waiting for its outcome: the response copied into resp, whose size *resp_size gives, and *resp_size
 * set to the bytes copied: 0 on every failure but ESP_ERR_INVALID_SIZE, what fits copied for a response bigger than
 * resp. resp or resp_size NULL takes none. Otherwise cyphal_call()'s error, or the outcome's (cyphal_response_cb_t).
 * ESP_ERR_NOT_ALLOWED, nothing sent, from a task the call would wait on: the node's task, which sends and receives
 * the call; its worker, which runs its callback (the application's poll, its services' work, a callback); or a task
 * holding the node's lock, which the worker takes (the allocator's table, a register written over Cyphal): an
 * application register's write hook (cyphal_register_t.write) run for the console's `cyphal register`. Nor while
 * holding anything else the worker may wait for. It waits as long as the call lasts, the timeout and the worker's turn
 */
esp_err_t cyphal_call_wait(size_t port, uint8_t server, const void *request, size_t size, void *resp,
                           size_t *resp_size, uint32_t timeout_ms);

void cyphal_client_status(cyphal_client_status_t *out);
#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
