/*
 * Host-built tests for cyphal/cyphal_client.c: the client's plain parts (the calls' records and states, the
 * transfer-IDs, a response matched to its call, calls timed out and failed), and the CYPHAL_PORT_CLI kind in the
 * register table (cyphal_registers.c built here with CONFIG_CYPHAL_CLIENT). Requests and responses go through
 * libcanard (third_party/libcanard), a client instance and server instances with the frames handed between them,
 * so a response reaches cyphal_call_match() as it reaches the node's task: single- and multi-frame, cut to the
 * subscription's extent, from the node called or another. What the node's task does around each step
 * (client_send(), on_response() in cyphal.c) is copied here in a few lines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canard.h"
#include "check.h"
#include "cyphal_client.h"
#include "cyphal_registers.h"
#include "values.h"
#include "uavcan/node/GetInfo_1_0.h"
#include "uavcan/node/Heartbeat_1_0.h"
#include "uavcan/node/port/List_1_0.h"

#define CALLS     4
#define PORTS     3             /* rows of transfer-IDs: the services the tests call */
#define MOTION    100           /* a service-ID the tests call (holo-player-fw's r2.holo.Motion) */
#define LIGHT     101
#define CLIENT_ID 20            /* the dome panel's */
#define HOLO_ID   100
#define OTHER_ID  101
#define S         1000000LL     /* a second, in microseconds */

/* ---- libcanard on the computer */

static void *mem_alloc(void *ref, size_t size)
{
    (void)ref;
    return malloc(size > 0 ? size : 1);
}

static void mem_free(void *ref, size_t size, void *p)
{
    (void)ref;
    (void)size;
    free(p);
}

static const struct CanardMemoryResource MEM = { NULL, mem_free, mem_alloc };

typedef struct {
    struct CanardInstance ins;
    struct CanardTxQueue txq;
    struct CanardRxSubscription subs[2];
} node_t;

/* A node on node_id: the client subscribed to responses on `ports` (with `extent`), a server to requests */
static void node_init(node_t *n, uint8_t node_id, enum CanardTransferKind kind, const uint16_t *ports, size_t count,
                      size_t extent)
{
    memset(n, 0, sizeof(*n));
    n->ins = canardInit(MEM);
    n->ins.node_id = node_id;
    n->txq = canardTxInit(200, CANARD_MTU_CAN_CLASSIC, MEM);
    for (size_t i = 0; i < count; i++) {
        CHECK(canardRxSubscribe(&n->ins, kind, ports[i], extent, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                &n->subs[i]) == 1);
    }
}

/* Every frame in `from`'s queue handed to `to`, in order, as the bus would: true with *out the transfer `to`
 * accepted (its payload the caller's to free), false for none. *frames: how many went */
static bool deliver(node_t *from, node_t *to, int64_t now, struct CanardRxTransfer *out, size_t *frames)
{
    bool got = false;
    size_t n = 0;
    struct CanardTxQueueItem *item;
    while ((item = canardTxPeek(&from->txq)) != NULL) {
        const struct CanardFrame f = { item->frame.extended_can_id,
                                       { item->frame.payload.size, item->frame.payload.data } };
        struct CanardRxTransfer t;
        struct CanardRxSubscription *sub = NULL;
        if (canardRxAccept(&to->ins, (CanardMicrosecond)now, &f, 0, &t, &sub) == 1) {
            if (got) {
                free(out->payload.data);    /* only the last is kept */
            }
            *out = t;
            got = true;
        }
        canardTxFree(&from->txq, &from->ins, canardTxPop(&from->txq, item));
        n++;
    }
    if (frames != NULL) {
        *frames = n;
    }
    return got;
}

/* The transfer-ID in the last frame queued: its tail byte's low five bits */
static uint8_t queued_tid(const node_t *n)
{
    const struct CanardTxQueueItem *item = canardTxPeek(&n->txq);
    return item == NULL ? 0xFF : (uint8_t)(((const uint8_t *)item->frame.payload.data)[item->frame.payload.size - 1] & 31);
}

/* Every frame in n's queue dropped, as if the bus had lost them: how many */
static size_t discard(node_t *n)
{
    size_t k = 0;
    struct CanardTxQueueItem *item;
    for (; (item = canardTxPeek(&n->txq)) != NULL; k++) {
        canardTxFree(&n->txq, &n->ins, canardTxPop(&n->txq, item));
    }
    return k;
}

/* ---- the node's task, as cyphal.c does it */

static cyphal_call_t s_records[CALLS];
static uint32_t s_states[CALLS];
static uint32_t s_tickets[CALLS];   /* each record's ticket, as call() last queued it */
static uint8_t s_tids[PORTS * CYPHAL_CALL_SERVERS];

static void pool(cyphal_call_pool_t *p)
{
    memset(s_records, 0xAA, sizeof(s_records));     /* nothing left over from before */
    cyphal_call_pool_init(p, s_records, s_states, CALLS, s_tids, PORTS);
}

/* cyphal_call(): a record taken, filled in and queued, its ticket kept in s_tickets[]. Its index */
static int call(cyphal_call_pool_t *p, size_t port, uint8_t server, const void *request, size_t size, int64_t deadline)
{
    const int i = cyphal_call_take(p);
    if (i < 0) {
        return i;
    }
    cyphal_call_t *c = &p->calls[i];
    c->port = port;
    c->server = server;
    c->deadline_us = deadline;
    c->cb = NULL;
    c->ctx = NULL;
    c->request_size = size;
    if (size > 0) {
        memcpy(c->request, request, size);
    }
    s_tickets[i] = cyphal_call_queue(p, (size_t)i);
    return i;
}

/* client_send(): the call queued, taken, its request queued at its transfer-ID with its transfer deadline. False if
 * its caller took it back */
static bool send_call(cyphal_call_pool_t *p, node_t *client, int i, uint16_t service_id, int64_t now)
{
    if (!cyphal_call_begin(p, (size_t)i, s_tickets[i])) {
        return false;
    }
    cyphal_call_t *c = &p->calls[i];
    const uint8_t tid = cyphal_call_next_tid(p, c->port, c->server, service_id);
    const struct CanardTransferMetadata meta = { CanardPriorityNominal, CanardTransferKindRequest, service_id, c->server,
                                                 tid };
    CHECK(canardTxPush(&client->txq, &client->ins, (CanardMicrosecond)cyphal_call_tx_deadline(p, (size_t)i, now, S),
                       &meta, (struct CanardPayload){ c->request_size, c->request }, 0, NULL) > 0);
    cyphal_call_sent(p, (size_t)i, service_id, tid);
    return true;
}

/* A server's answer to the request it took: the same service, priority and transfer-ID, back to the caller */
static void answer(node_t *server, const struct CanardRxTransfer *rq, const void *data, size_t size, int64_t now)
{
    const struct CanardTransferMetadata meta = { rq->metadata.priority, CanardTransferKindResponse, rq->metadata.port_id,
                                                 rq->metadata.remote_node_id, rq->metadata.transfer_id };
    CHECK(canardTxPush(&server->txq, &server->ins, (CanardMicrosecond)(now + S), &meta,
                       (struct CanardPayload){ size, data }, 0, NULL) > 0);
}

/* on_response(): the response the client accepted, matched (its payload freed, as on_frame() does): the call's index,
 * or -1 for none */
static int take_response(cyphal_call_pool_t *p, node_t *server, node_t *client, int64_t now, size_t *frames)
{
    struct CanardRxTransfer t;
    if (!deliver(server, client, now, &t, frames)) {
        return -2;      /* libcanard took nothing */
    }
    CHECK(t.metadata.transfer_kind == CanardTransferKindResponse);
    const int i = cyphal_call_match(p, t.metadata.port_id, (uint8_t)t.metadata.remote_node_id, t.metadata.transfer_id,
                                    t.payload.data, t.payload.size);
    free(t.payload.data);
    return i;
}

/* ---- the tests */

static void test_pool(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    CHECK(p.count == CALLS && cyphal_call_held(&p) == 0 && cyphal_call_done(&p) == -1 && !cyphal_call_waiting(&p));
    CHECK(cyphal_call_next_deadline(&p) == INT64_MAX);
    /* Taken one by one, then none: the pool full */
    for (int i = 0; i < CALLS; i++) {
        CHECK(cyphal_call_take(&p) == i);
    }
    CHECK(cyphal_call_take(&p) == -1);
    CHECK(cyphal_call_held(&p) == CALLS);
    /* Every one given back (as the worker does after a callback): the next take gets the first */
    for (int i = 0; i < CALLS; i++) {
        cyphal_call_free(&p, (size_t)i);
    }
    CHECK(cyphal_call_held(&p) == 0 && cyphal_call_take(&p) == 0);
    cyphal_call_free(&p, 0);
    /* Queued, then taken back before the node's task took it: free, and never counted as made */
    const uint8_t rq[3] = { 1, 2, 3 };
    const int i = call(&p, 0, HOLO_ID, rq, sizeof(rq), 5 * S);
    uint32_t ticket = 0;
    CHECK(i == 0 && cyphal_call_held(&p) == 1 && cyphal_call_waiting(&p) && cyphal_call_next_queued(&p, &ticket) == 0);
    CHECK(ticket == s_tickets[0] && CYPHAL_CALL_STATE(ticket) == CYPHAL_CALL_QUEUED && ticket == s_states[0]);
    CHECK(cyphal_call_next_deadline(&p) == 5 * S);
    CHECK(cyphal_call_cancel(&p, (size_t)i, s_tickets[i]));
    CHECK(p.counts.made == 0 && cyphal_call_held(&p) == 0 && !cyphal_call_waiting(&p));
    CHECK(!cyphal_call_cancel(&p, (size_t)i, s_tickets[i]));    /* free: nothing to take back */
    /* Queued, taken by the node's task first: counted as made then; the caller can't take it back, and its end is
     * the node's */
    CHECK(call(&p, 0, HOLO_ID, rq, sizeof(rq), 5 * S) == 0 && p.counts.made == 0);
    CHECK(cyphal_call_begin(&p, 0, s_tickets[0]) && p.counts.made == 1);
    CHECK(!cyphal_call_cancel(&p, 0, s_tickets[0]) && p.counts.made == 1);
    CHECK(!cyphal_call_begin(&p, 0, s_tickets[0]) && p.counts.made == 1);     /* once only */
    CHECK(!cyphal_call_begin(&p, 0, s_states[0]) && p.counts.made == 1);      /* a ticket names a queued call only */
    cyphal_call_finish(&p, 0, CYPHAL_CALL_FAILED);
    CHECK(cyphal_call_done(&p) == 0 && p.counts.failed == 1 && s_records[0].response_size == 0);
    CHECK(cyphal_call_held(&p) == 1);               /* done, until the worker frees it */
    cyphal_call_free(&p, 0);
    CHECK(cyphal_call_held(&p) == 0);
    /* Counts cleared */
    cyphal_call_counts_clear(&p);
    CHECK(p.counts.made == 0 && p.counts.failed == 0);
    /* No memory: a pool of none */
    cyphal_call_pool_t none;
    cyphal_call_pool_init(&none, NULL, s_states, CALLS, NULL, PORTS);
    CHECK(none.count == 0 && cyphal_call_take(&none) == -1 && cyphal_call_next_tid(&none, 0, 1, MOTION) == 0);
}

/* With no record held, every look at the records returns at once, without reading their states: shown here by states
 * set behind the pool's back, which a scan would find */
static void test_none_held(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    s_records[0].deadline_us = 0;
    s_records[1].deadline_us = 5 * S;
    s_states[0] = CYPHAL_CALL_QUEUED;
    s_states[1] = CYPHAL_CALL_SENT;
    s_states[2] = CYPHAL_CALL_DONE;
    CHECK(cyphal_call_held(&p) == 0 && !cyphal_call_waiting(&p) && cyphal_call_next_queued(&p, NULL) == -1);
    CHECK(cyphal_call_next_deadline(&p) == INT64_MAX && cyphal_call_expire(&p, S) == 0 && cyphal_call_done(&p) == -1);
    CHECK(cyphal_call_fail_all(&p) == 0 && cyphal_call_match(&p, MOTION, HOLO_ID, 0, NULL, 0) == -1);
    CHECK(p.counts.made == 0 && p.counts.timed_out == 0 && p.counts.failed == 0 && p.counts.dropped == 1);
    /* One held (taken, being filled): the looks read the states again, and find what was set */
    CHECK(cyphal_call_take(&p) == 3 && cyphal_call_held(&p) == 1);
    CHECK(cyphal_call_waiting(&p) && cyphal_call_done(&p) == 2 && cyphal_call_next_deadline(&p) == 0);
    memset(s_states, 0, sizeof(s_states));
}

static void test_answered(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION, LIGHT };
    node_t client, holo;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 2, CYPHAL_CALL_RESPONSE_MAX);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 2, 64);
    int64_t now = 10 * S;

    /* A request of 3 bytes, a response of 5: a frame each way */
    const uint8_t rq[3] = { 7, 8, 9 };
    const int i = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + S);
    CHECK(send_call(&p, &client, i, MOTION, now));
    CHECK(!cyphal_call_waiting(&p) && cyphal_call_next_deadline(&p) == now + S);
    struct CanardRxTransfer t;
    size_t frames = 0;
    CHECK(deliver(&client, &holo, now, &t, &frames) && frames == 1);
    CHECK(t.metadata.transfer_kind == CanardTransferKindRequest && t.metadata.port_id == MOTION &&
          t.metadata.remote_node_id == CLIENT_ID && t.payload.size == 3 && memcmp(t.payload.data, rq, 3) == 0);
    const uint8_t rs[5] = { 0, 1, 2, 3, 4 };
    answer(&holo, &t, rs, sizeof(rs), now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 1000, &frames) == i && frames == 1);
    CHECK(cyphal_call_done(&p) == i && s_records[i].outcome == CYPHAL_CALL_ANSWERED);
    CHECK(s_records[i].response_size == 5 && memcmp(s_records[i].response, rs, 5) == 0);
    CHECK(p.counts.answered == 1 && p.counts.made == 1 && p.counts.dropped == 0);
    CHECK(cyphal_call_next_deadline(&p) == INT64_MAX);      /* answered: nothing to time out */
    cyphal_call_free(&p, (size_t)i);

    /* An empty request (uavcan.node.GetInfo's), and a multi-frame response: a GetInfo response with the holo's name,
     * 51 bytes in 8 frames, read back whole */
    uavcan_node_GetInfo_Response_1_0 info;
    memset(&info, 0, sizeof(info));
    info.protocol_version.major = 1;
    info.software_version.major = 1;
    info.software_version.minor = 1;
    memset(info.unique_id, 0x5A, sizeof(info.unique_id));
    info.name.count = strlen("r2.dome.holo.front");
    memcpy(info.name.elements, "r2.dome.holo.front", info.name.count);
    uint8_t buf[uavcan_node_GetInfo_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t size = sizeof(buf);
    CHECK(uavcan_node_GetInfo_Response_1_0_serialize_(&info, buf, &size) == 0 && size == 51);
    const int j = call(&p, 1, HOLO_ID, NULL, 0, now + S);
    CHECK(send_call(&p, &client, j, LIGHT, now));
    CHECK(deliver(&client, &holo, now, &t, &frames) && frames == 1 && t.payload.size == 0);
    answer(&holo, &t, buf, size, now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 2000, &frames) == j && frames == cyphal_frames(51) && frames == 8);
    CHECK(s_records[j].response_size == 51 && memcmp(s_records[j].response, buf, 51) == 0);
    uavcan_node_GetInfo_Response_1_0 back;
    size_t n = s_records[j].response_size;
    CHECK(uavcan_node_GetInfo_Response_1_0_deserialize_(&back, s_records[j].response, &n) == 0 && back.name.count == 18 &&
          memcmp(back.name.elements, "r2.dome.holo.front", 18) == 0);
    cyphal_call_free(&p, (size_t)j);

    /* A response of the largest extent kept, 448 bytes in 65 frames: all of it */
    static uint8_t big[CYPHAL_CALL_RESPONSE_MAX + 100];
    for (size_t k = 0; k < sizeof(big); k++) {
        big[k] = (uint8_t)(k * 7);
    }
    const int k = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(send_call(&p, &client, k, MOTION, now));
    CHECK(deliver(&client, &holo, now, &t, NULL));
    answer(&holo, &t, big, CYPHAL_CALL_RESPONSE_MAX, now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 3000, &frames) == k && frames == 65);
    CHECK(s_records[k].response_size == CYPHAL_CALL_RESPONSE_MAX &&
          memcmp(s_records[k].response, big, CYPHAL_CALL_RESPONSE_MAX) == 0);
    cyphal_call_free(&p, (size_t)k);

    /* A longer one than the subscription's extent: libcanard cuts it there (here the largest kept) */
    const int m = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(send_call(&p, &client, m, MOTION, now));
    CHECK(deliver(&client, &holo, now, &t, NULL));
    answer(&holo, &t, big, sizeof(big), now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 4000, NULL) == m);
    CHECK(s_records[m].response_size == CYPHAL_CALL_RESPONSE_MAX &&
          memcmp(s_records[m].response, big, CYPHAL_CALL_RESPONSE_MAX) == 0);
    cyphal_call_free(&p, (size_t)m);
    CHECK(p.counts.answered == 4 && p.counts.timed_out == 0 && p.counts.dropped == 0 && cyphal_call_held(&p) == 0);
}

static void test_timeout_and_late(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION };
    node_t client, holo;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 1, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 1, 64);
    const int64_t now = 20 * S;
    const uint8_t rq[2] = { 1, 2 };
    const uint8_t rs[4] = { 9, 9, 9, 9 };

    const int i = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + S);
    CHECK(send_call(&p, &client, i, MOTION, now));
    struct CanardRxTransfer t;
    CHECK(deliver(&client, &holo, now, &t, NULL));
    /* Not yet; then at its deadline: timed out, with no response */
    CHECK(cyphal_call_expire(&p, now + S - 1) == 0 && cyphal_call_done(&p) == -1);
    CHECK(cyphal_call_expire(&p, now + S) == 1);
    CHECK(cyphal_call_done(&p) == i && s_records[i].outcome == CYPHAL_CALL_TIMED_OUT && s_records[i].response_size == 0);
    CHECK(p.counts.timed_out == 1 && p.counts.answered == 0);
    CHECK(cyphal_call_expire(&p, now + 2 * S) == 0);    /* once only */
    /* The answer comes late, while the callback is still to run, and after: dropped, counted, both times */
    answer(&holo, &t, rs, sizeof(rs), now);
    CHECK(take_response(&p, &holo, &client, now + S + 100, NULL) == -1 && p.counts.dropped == 1);
    cyphal_call_free(&p, (size_t)i);
    answer(&holo, &t, rs, sizeof(rs), now);     /* the same transfer again: libcanard takes it for a repeat */
    CHECK(take_response(&p, &holo, &client, now + S + 200, NULL) == -2 && p.counts.dropped == 1);
    free(t.payload.data);

    /* A call that never went (no room, say) times out queued; the node's task never sends it */
    const int j = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + 3 * S);
    CHECK(cyphal_call_next_deadline(&p) == now + 3 * S);
    CHECK(cyphal_call_expire(&p, now + 3 * S) == 1 && s_records[j].outcome == CYPHAL_CALL_TIMED_OUT);
    CHECK(!cyphal_call_begin(&p, (size_t)j, s_tickets[j]));
    cyphal_call_free(&p, (size_t)j);
    /* The next call answered as usual: a new transfer-ID, which neither libcanard on either side takes for a repeat */
    const int k = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + 5 * S);
    CHECK(send_call(&p, &client, k, MOTION, now + 4 * S) && s_records[k].transfer_id == 1);
    CHECK(deliver(&client, &holo, now + 4 * S, &t, NULL) && t.metadata.transfer_id == 1);
    answer(&holo, &t, rs, sizeof(rs), now + 4 * S);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 4 * S + 100, NULL) == k && p.counts.answered == 1);
    cyphal_call_free(&p, (size_t)k);
}

static void test_wrong_source(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION };
    node_t client, holo, other;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 1, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 1, 64);
    node_init(&other, OTHER_ID, CanardTransferKindRequest, ports, 1, 64);
    const int64_t now = 30 * S;
    const uint8_t rq[1] = { 5 };
    const uint8_t rs[2] = { 6, 6 };

    const int i = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + S);
    CHECK(send_call(&p, &client, i, MOTION, now));
    struct CanardRxTransfer t;
    CHECK(deliver(&client, &holo, now, &t, NULL));
    /* Node 101 never got the request (it was for 100), but answers as if: the same service and transfer-ID, to us.
     * libcanard takes it (it is to us), and nothing matches it */
    answer(&other, &t, rs, sizeof(rs), now);
    CHECK(take_response(&p, &other, &client, now + 100, NULL) == -1 && p.counts.dropped == 1);
    CHECK(cyphal_call_done(&p) == -1);
    /* The holo answers with another transfer-ID (a stale one): nothing matches */
    struct CanardRxTransfer stale = t;
    stale.metadata.transfer_id = (uint8_t)((t.metadata.transfer_id + 5) % 32);
    answer(&holo, &stale, rs, sizeof(rs), now);
    CHECK(take_response(&p, &holo, &client, now + 200, NULL) == -1 && p.counts.dropped == 2);
    /* Then the right answer: the call's */
    answer(&holo, &t, rs, sizeof(rs), now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 300, NULL) == i && p.counts.answered == 1);
    /* A response on another service with the call's transfer-ID, from the holo: not its call */
    CHECK(cyphal_call_match(&p, LIGHT, HOLO_ID, s_records[i].transfer_id, rs, sizeof(rs)) == -1 && p.counts.dropped == 3);
    cyphal_call_free(&p, (size_t)i);
}

static void test_restart(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION };
    node_t client, holo;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 1, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 1, 64);
    const int64_t now = 40 * S;
    const uint8_t rq[1] = { 1 };
    const uint8_t rs[1] = { 2 };

    /* One sent, one queued, one being sent, one a caller is filling in */
    const int sent = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(send_call(&p, &client, sent, MOTION, now));
    struct CanardRxTransfer t;
    CHECK(deliver(&client, &holo, now, &t, NULL));
    const int queued = call(&p, 0, HOLO_ID, rq, 1, now + S);
    const int sending = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(cyphal_call_begin(&p, (size_t)sending, s_tickets[sending]));
    const int filling = cyphal_call_take(&p);
    CHECK(sent == 0 && queued == 1 && sending == 2 && filling == 3);
    /* The node stops (or restarts, or loses its node-ID): the three out failed; the one being filled is its caller's */
    CHECK(cyphal_call_fail_all(&p) == 3 && p.counts.failed == 3);
    for (int i = 0; i < 3; i++) {
        CHECK(CYPHAL_CALL_STATE(s_states[i]) == CYPHAL_CALL_DONE && s_records[i].outcome == CYPHAL_CALL_FAILED);
    }
    CHECK(CYPHAL_CALL_STATE(s_states[filling]) == CYPHAL_CALL_FILLING);
    CHECK(cyphal_call_next_deadline(&p) == INT64_MAX && cyphal_call_expire(&p, now + 10 * S) == 0);
    /* That caller queues it and finds the node no longer online: it takes it back (no callback) */
    s_records[filling].request_size = 0;
    const uint32_t ticket = cyphal_call_queue(&p, (size_t)filling);
    CHECK(cyphal_call_cancel(&p, (size_t)filling, ticket));
    /* The answer to the call that was sent arrives after the stop: dropped */
    answer(&holo, &t, rs, sizeof(rs), now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 100, NULL) == -1 && p.counts.dropped == 1);
    /* The worker runs the three callbacks and frees them; a second stop finds nothing */
    for (int i = cyphal_call_done(&p); i >= 0; i = cyphal_call_done(&p)) {
        cyphal_call_free(&p, (size_t)i);
    }
    CHECK(cyphal_call_held(&p) == 0 && cyphal_call_fail_all(&p) == 0);
    /* A request libcanard refused: not sent */
    const int i = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(cyphal_call_begin(&p, (size_t)i, s_tickets[i]));
    cyphal_call_finish(&p, (size_t)i, CYPHAL_CALL_NOT_SENT);
    CHECK(p.counts.not_sent == 1 && cyphal_call_done(&p) == i);
    cyphal_call_free(&p, (size_t)i);
    CHECK(p.counts.made == 4 && p.counts.answered + p.counts.timed_out + p.counts.not_sent + p.counts.failed == 4);
}

static void test_tids(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION, LIGHT };
    node_t client, holo, other;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 2, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 2, 64);
    node_init(&other, OTHER_ID, CanardTransferKindRequest, ports, 2, 64);
    int64_t now = 50 * S;
    const uint8_t rq[1] = { 3 };
    const uint8_t rs[1] = { 4 };

    /* One counter for each port and server: 0, 1, 2 to the holo on port 0; 0 to another node, and on another port */
    CHECK(cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 0 && cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 1);
    CHECK(cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 2);
    CHECK(cyphal_call_next_tid(&p, 0, OTHER_ID, MOTION) == 0 && cyphal_call_next_tid(&p, 1, HOLO_ID, LIGHT) == 0);
    CHECK(cyphal_call_next_tid(&p, 2, 127, 102) == 0 && cyphal_call_next_tid(&p, PORTS, 1, 103) == 0);  /* past: 0 */
    /* Modulo 32 */
    for (int k = 0; k < 29; k++) {
        cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION);
    }
    CHECK(cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 0);
    /* Not cleared with the counts, nor by a stop: a server never sees one repeat the one before */
    cyphal_call_counts_clear(&p);
    cyphal_call_fail_all(&p);
    CHECK(cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 1);

    /* On the wire: 40 calls in a row to the holo on port 1, every one taken by the holo, each one's transfer-ID in its
     * frame's tail byte and one more than the last, and every answer matched; between them, calls to another node on
     * the same port, which don't move the holo's counter */
    uint8_t want = 1;       /* port 1's counter for the holo: 0 was used above */
    for (int k = 0; k < 40; k++) {
        const int i = call(&p, 1, HOLO_ID, rq, 1, now + S);
        CHECK(send_call(&p, &client, i, LIGHT, now));
        CHECK(queued_tid(&client) == want && s_records[i].transfer_id == want);
        struct CanardRxTransfer t;
        CHECK(deliver(&client, &holo, now, &t, NULL) && t.metadata.transfer_id == want);
        answer(&holo, &t, rs, 1, now);
        free(t.payload.data);
        CHECK(take_response(&p, &holo, &client, now + 10, NULL) == i);
        cyphal_call_free(&p, (size_t)i);
        if (k % 3 == 0) {
            const int o = call(&p, 1, OTHER_ID, rq, 1, now + S);
            CHECK(send_call(&p, &client, o, LIGHT, now));
            CHECK(deliver(&client, &other, now, &t, NULL));
            answer(&other, &t, rs, 1, now);
            free(t.payload.data);
            CHECK(take_response(&p, &other, &client, now + 20, NULL) == o);
            cyphal_call_free(&p, (size_t)o);
        }
        want = (uint8_t)((want + 1) % 32);
        now += 1000;        /* well inside libcanard's 2 s transfer-ID timeout: a repeat would be refused */
    }
    CHECK(p.counts.answered == 40 + 14 && p.counts.dropped == 0);
}

/* A transfer-ID a call still out holds is not given again on its service and server: 32 calls after one the holo
 * never answered (its request lost), the counter is back at that call's, which is skipped, and every answer goes to
 * its own call. Before, the 33rd took the same transfer-ID and its answer went to the first, the lower record */
static void test_tid_held(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION };
    node_t client, holo;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 1, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 1, 64);
    int64_t now = 60 * S;
    const uint8_t rq[1] = { 1 };
    struct CanardRxTransfer t;

    const int stuck = call(&p, 0, HOLO_ID, rq, 1, now + 10 * S);
    CHECK(send_call(&p, &client, stuck, MOTION, now) && s_records[stuck].transfer_id == 0);
    CHECK(discard(&client) == 1);
    /* 31 more, each answered at once: transfer-IDs 1 to 31 */
    for (uint8_t k = 1; k < 32; k++) {
        const int i = call(&p, 0, HOLO_ID, rq, 1, now + S);
        CHECK(send_call(&p, &client, i, MOTION, now) && s_records[i].transfer_id == k);
        CHECK(deliver(&client, &holo, now, &t, NULL));
        const uint8_t rs[1] = { k };
        answer(&holo, &t, rs, 1, now);
        free(t.payload.data);
        CHECK(take_response(&p, &holo, &client, now + 10, NULL) == i && s_records[i].response[0] == k);
        cyphal_call_free(&p, (size_t)i);
        now += 1000;
    }
    /* The next would be 0, the stuck call's: 1 instead, which the holo takes (its last was 31), and its answer is its */
    const int next = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(next != stuck && send_call(&p, &client, next, MOTION, now));
    CHECK(s_records[next].transfer_id == 1 && queued_tid(&client) == 1);
    CHECK(deliver(&client, &holo, now, &t, NULL) && t.metadata.transfer_id == 1);
    const uint8_t rs[1] = { 99 };
    answer(&holo, &t, rs, 1, now);
    free(t.payload.data);
    CHECK(take_response(&p, &holo, &client, now + 10, NULL) == next && s_records[next].response[0] == 99);
    CHECK(CYPHAL_CALL_STATE(s_states[stuck]) == CYPHAL_CALL_SENT && p.counts.dropped == 0);
    cyphal_call_free(&p, (size_t)next);
    /* Held on its own service and server only: to another node, or on another service, 0 is given */
    CHECK(cyphal_call_next_tid(&p, 0, OTHER_ID, MOTION) == 0 && cyphal_call_next_tid(&p, 1, HOLO_ID, LIGHT) == 0);
    /* Once the stuck call has timed out, its transfer-ID is free again: round to 0 and it is given */
    CHECK(cyphal_call_expire(&p, now + 10 * S) == 1 && cyphal_call_done(&p) == stuck);
    cyphal_call_free(&p, (size_t)stuck);
    for (int k = 2; k < 32; k++) {
        cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION);
    }
    CHECK(cyphal_call_next_tid(&p, 0, HOLO_ID, MOTION) == 0);
    /* Every record but the one being sent out with a transfer-ID held: the next free one is given */
    for (uint8_t k = 0; k < CALLS - 1; k++) {
        const int i = call(&p, 0, HOLO_ID, rq, 1, now + S);
        CHECK(send_call(&p, &client, i, MOTION, now) && s_records[i].transfer_id == k + 1);
    }
    s_tids[0 * CYPHAL_CALL_SERVERS + HOLO_ID] = 1;     /* the counter at the first of them */
    const int last = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(send_call(&p, &client, last, MOTION, now) && s_records[last].transfer_id == CALLS);
    discard(&client);
}

/* A record's generation: a caller's cancel late enough to find its record failed by a stop, freed by the worker and
 * taken by another call leaves that call alone, and a begin with a ticket from before the record changed hands takes
 * nothing. Before, the late cancel freed the other caller's call, whose callback then never came */
static void test_generation(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint8_t rq[2] = { 4, 5 };
    const int64_t now = 70 * S;

    const int a = call(&p, 0, HOLO_ID, rq, 1, now + S);
    const uint32_t ticket_a = s_tickets[a];
    CHECK(cyphal_call_fail_all(&p) == 1 && cyphal_call_done(&p) == a);
    cyphal_call_free(&p, (size_t)a);
    const int b = call(&p, 1, OTHER_ID, rq, 2, now + 2 * S);
    CHECK(b == a && s_tickets[b] != ticket_a && CYPHAL_CALL_STATE(s_tickets[b]) == CYPHAL_CALL_QUEUED);
    CHECK(!cyphal_call_cancel(&p, (size_t)a, ticket_a));
    CHECK(cyphal_call_held(&p) == 1 && cyphal_call_next_queued(&p, NULL) == b);
    CHECK(!cyphal_call_begin(&p, (size_t)b, ticket_a) && p.counts.made == 1);
    /* Its own ticket begins it, and what the node's task reads then is its call */
    CHECK(cyphal_call_begin(&p, (size_t)b, s_tickets[b]) && p.counts.made == 2);
    CHECK(s_records[b].port == 1 && s_records[b].server == OTHER_ID && s_records[b].request_size == 2);
    cyphal_call_finish(&p, (size_t)b, CYPHAL_CALL_FAILED);
    cyphal_call_free(&p, (size_t)b);
    /* A call's own cancel works while it is queued; a second take of the record counts the generation on again */
    const int c = call(&p, 0, HOLO_ID, rq, 1, now + S);
    const uint32_t ticket_c = s_tickets[c];
    CHECK(c == a && cyphal_call_cancel(&p, (size_t)c, ticket_c) && cyphal_call_held(&p) == 0);
    const int d = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(d == a && s_tickets[d] != ticket_c && !cyphal_call_cancel(&p, (size_t)d, ticket_c));
    CHECK(cyphal_call_cancel(&p, (size_t)d, s_tickets[d]));
    /* The generation wraps with the word, and the state stays in its bits */
    s_states[0] = UINT32_MAX & ~((1u << CYPHAL_CALL_STATE_BITS) - 1);   /* FREE, the last generation */
    const int e = call(&p, 0, HOLO_ID, rq, 1, now + S);
    CHECK(e == 0 && CYPHAL_CALL_STATE(s_states[0]) == CYPHAL_CALL_QUEUED && s_states[0] >> CYPHAL_CALL_STATE_BITS == 0);
    CHECK(cyphal_call_cancel(&p, 0, s_tickets[0]) && cyphal_call_held(&p) == 0);
}

/* Order: calls are taken by the node's task oldest first, and the worker's callbacks run oldest first, whichever
 * records they are in. Before, the lowest record went first both times */
static void test_order(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    const uint16_t ports[] = { MOTION };
    node_t client, holo;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, ports, 1, 128);
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, ports, 1, 64);
    const int64_t now = 80 * S;

    /* Every record taken; the last freed, then the second: the older call lands on 3, the younger on 1 */
    for (int k = 0; k < CALLS; k++) {
        CHECK(cyphal_call_take(&p) == k);
    }
    cyphal_call_free(&p, 3);
    const uint8_t first[1] = { 1 }, second[1] = { 2 };
    const int older = call(&p, 0, HOLO_ID, first, 1, now + S);
    cyphal_call_free(&p, 1);
    const int younger = call(&p, 0, HOLO_ID, second, 1, now + S);
    CHECK(older == 3 && younger == 1);
    uint32_t ticket = 0;
    CHECK(cyphal_call_next_queued(&p, &ticket) == older && ticket == s_tickets[older]);
    /* client_send()'s loop: the oldest each time */
    int sent[2] = { -1, -1 };
    for (int k = 0, i; k < 2 && (i = cyphal_call_next_queued(&p, NULL)) >= 0; k++) {
        CHECK(send_call(&p, &client, i, MOTION, now));
        sent[k] = i;
    }
    CHECK(sent[0] == older && sent[1] == younger && cyphal_call_next_queued(&p, NULL) == -1);
    /* The holo takes them in that order and answers each: both ended before the worker's turn, the older's first */
    for (int k = 0; k < 2; k++) {
        struct CanardTxQueueItem *item = canardTxPeek(&client.txq);
        CHECK(item != NULL && ((const uint8_t *)item->frame.payload.data)[0] == (k == 0 ? 1 : 2));
        const struct CanardFrame f = { item->frame.extended_can_id, { item->frame.payload.size, item->frame.payload.data } };
        struct CanardRxTransfer t;
        struct CanardRxSubscription *sub = NULL;
        CHECK(canardRxAccept(&holo.ins, (CanardMicrosecond)now, &f, 0, &t, &sub) == 1);
        canardTxFree(&client.txq, &client.ins, canardTxPop(&client.txq, item));
        answer(&holo, &t, t.payload.data, 1, now);
        free(t.payload.data);
    }
    CHECK(take_response(&p, &holo, &client, now + 10, NULL) == younger);   /* deliver() keeps the last: the younger */
    CHECK(cyphal_call_match(&p, MOTION, HOLO_ID, s_records[older].transfer_id, first, 1) == older);
    CHECK(cyphal_call_done(&p) == older);
    cyphal_call_free(&p, (size_t)older);
    CHECK(cyphal_call_done(&p) == younger && s_records[younger].response[0] == 2);
    cyphal_call_free(&p, (size_t)younger);
    /* A call taken and given back for want of room keeps its place, and is uncounted */
    for (int k = 0; k < CALLS; k++) {
        if (CYPHAL_CALL_STATE(s_states[k]) == CYPHAL_CALL_FILLING) {
            cyphal_call_free(&p, (size_t)k);
        }
    }
    const int x = call(&p, 0, HOLO_ID, first, 1, now + S);
    const int y = call(&p, 0, HOLO_ID, second, 1, now + S);
    const uint32_t made = p.counts.made;
    CHECK(cyphal_call_next_queued(&p, &ticket) == x && cyphal_call_begin(&p, (size_t)x, ticket));
    cyphal_call_hold(&p, (size_t)x);
    CHECK(p.counts.made == made && cyphal_call_next_queued(&p, &ticket) == x && ticket == s_tickets[x]);
    CHECK(cyphal_call_cancel(&p, (size_t)x, s_tickets[x]));     /* its caller's ticket still names it */
    CHECK(cyphal_call_next_queued(&p, NULL) == y && cyphal_call_cancel(&p, (size_t)y, s_tickets[y]));
    /* The sequence wraps: one stamped just before the wrap is older than one just after, on a lower record */
    p.next_seq = UINT32_MAX;
    CHECK(cyphal_call_take(&p) == 0);
    const int before = call(&p, 0, HOLO_ID, first, 1, now + S);
    cyphal_call_free(&p, 0);
    const int after = call(&p, 0, HOLO_ID, second, 1, now + S);
    CHECK(before == 1 && after == 0 && s_records[after].seq == 0);
    CHECK(cyphal_call_next_queued(&p, NULL) == before);
    CHECK(cyphal_call_cancel(&p, (size_t)before, s_tickets[before]) && cyphal_call_cancel(&p, 0, s_tickets[0]));
    CHECK(cyphal_call_held(&p) == 0);
    discard(&client);
}

/* A request's transfer deadline: just before its call's deadline when that comes within a heartbeat period, so that
 * none of it is handed to the controller once the call has timed out; a period from now otherwise. Every frame of
 * the transfer carries it. Before, every request had a period */
static void test_tx_deadline(void)
{
    cyphal_call_pool_t p;
    pool(&p);
    node_t client;
    node_init(&client, CLIENT_ID, CanardTransferKindResponse, NULL, 0, 0);
    const int64_t now = 90 * S;
    static uint8_t rq[200];

    const int i = call(&p, 0, HOLO_ID, rq, 1, now + 50000);
    CHECK(cyphal_call_tx_deadline(&p, (size_t)i, now, S) == now + 50000 - 1);
    CHECK(send_call(&p, &client, i, MOTION, now));
    const struct CanardTxQueueItem *item = canardTxPeek(&client.txq);
    CHECK(item != NULL && (int64_t)item->tx_deadline_usec == now + 50000 - 1);
    discard(&client);
    const int j = call(&p, 0, HOLO_ID, rq, sizeof(rq), now + 5 * S);
    CHECK(cyphal_call_tx_deadline(&p, (size_t)j, now, S) == now + S);
    CHECK(send_call(&p, &client, j, MOTION, now));
    size_t frames = 0;
    for (item = canardTxPeek(&client.txq); item != NULL; item = item->next_in_transfer, frames++) {
        CHECK((int64_t)item->tx_deadline_usec == now + S);
    }
    CHECK(frames == cyphal_frames(sizeof(rq)));
    discard(&client);
    /* Already past (it ran out between the timers and the send): a deadline in the past, which the queue drops */
    const int k = call(&p, 1, HOLO_ID, rq, 1, now - 1);
    CHECK(cyphal_call_tx_deadline(&p, (size_t)k, now, S) < now);
}

/* The CYPHAL_PORT_CLI kind in the register table: `uavcan.cln.<name>.id`, a service-ID, and its own set */
static const cyphal_port_t PANEL[] = {
    { CYPHAL_PORT_SUB, "status", "r2.holo.Status.0.1", 2300, CYPHAL_PRIORITY_LOW, "uavcan.sub.status.id",
      "uavcan.sub.status.type", "sub.status" },
    { CYPHAL_PORT_PUB, "setpoint", "r2.holo.Axes.0.1", 2303, CYPHAL_PRIORITY_FAST, "uavcan.pub.setpoint.id",
      "uavcan.pub.setpoint.type", "pub.setpoint" },
    { CYPHAL_PORT_CLI, "motion", "r2.holo.Motion.0.1", 100, CYPHAL_PRIORITY_NOMINAL, "uavcan.cln.motion.id",
      "uavcan.cln.motion.type", "cln.motion" },
    { CYPHAL_PORT_CLI, "light", "r2.holo.Light.0.1", 101, CYPHAL_PRIORITY_NOMINAL, "uavcan.cln.light.id",
      "uavcan.cln.light.type", "cln.light" },
    { CYPHAL_PORT_SRV, "echo", "r2.test.Echo.0.1", 100, CYPHAL_PRIORITY_NOMINAL, "uavcan.srv.echo.id",
      "uavcan.srv.echo.type", "srv.echo" },
};
#define PANEL_N (sizeof(PANEL) / sizeof(PANEL[0]))
enum { P_STATUS, P_SETPOINT, P_MOTION, P_LIGHT, P_ECHO };

/* The node's own, as cyphal.c gives them with the client: it serves GetInfo (cyphal_reg_fixed()) and calls it
 * (FIXED_CLIENTS[], through cyphal_reg_called()) */
static const cyphal_fixed_port_t FIXED[] = {
    { CYPHAL_PORT_PUB, 7509 }, { CYPHAL_PORT_SRV, 430 }, { CYPHAL_PORT_SRV, 384 },
};

static bool called(uint16_t id)
{
    return id == 430 || id == 384 || id == 406;
}

static void test_registers(void)
{
    char why[160];
    cyphal_reg_fixed(FIXED, sizeof(FIXED) / sizeof(FIXED[0]));
    cyphal_reg_called(called);
    /* Motion called and echo served on 100: different sets */
    CHECK(cyphal_reg_init(PANEL, PANEL_N, "r2.node.name", NULL, 0, why, sizeof(why)));
    const int id = cyphal_reg_find((const uint8_t *)"uavcan.cln.motion.id", strlen("uavcan.cln.motion.id"));
    CHECK(id == 3 + 2 * P_MOTION && cyphal_reg_mutable((size_t)id) && !cyphal_reg_mutable((size_t)id + 1));
    CHECK(strcmp(cyphal_reg_name((size_t)id + 1), "uavcan.cln.motion.type") == 0);
    char type[24];
    CHECK(strcmp(cyphal_reg_type((size_t)id, type, sizeof(type)), "natural16[1]") == 0);
    /* A service-ID: 0 to 511 */
    CHECK(cyphal_port_id_valid(CYPHAL_PORT_CLI, 511) && !cyphal_port_id_valid(CYPHAL_PORT_CLI, 512));
    CHECK(cyphal_port_id_valid(CYPHAL_PORT_CLI, CYPHAL_PORT_OFF) && cyphal_port_id_valid(CYPHAL_PORT_SUB, 8191));
    cyphal_settings_t s;
    memset(&s, 0, sizeof(s));
    cyphal_ports_default(&s);
    CHECK(s.port_id[P_MOTION] == 100 && s.port_id[P_ECHO] == 100);
    uavcan_register_Value_1_0 v = nat16(1, 512);
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE &&
          strcmp(why, "a service-ID is 0 to 511, or 65535 to turn the port off") == 0);
    /* Taken by the other port called, or by a service the node itself calls; not by one it serves, or by a subject */
    v = nat16(1, 101);
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE &&
          strcmp(why, "service 101 is uavcan.cln.light.id's") == 0);
    v = nat16(1, 406);
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE &&
          strstr(why, "service 406 is one of the node's own ports") != NULL);
    cyphal_reg_called(NULL);    /* without the test, a service the node calls is free */
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[P_MOTION] == 406);
    cyphal_reg_called(called);
    v = nat16(1, 385);     /* served by the node (List), not called by it */
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[P_MOTION] == 385);
    v = nat16(1, 2300);    /* status's subject-ID, but too big for a service anyway */
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);
    v = nat16(1, 300);
    CHECK(cyphal_reg_write(&s, (size_t)id, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[P_MOTION] == 300);
    /* The served echo may take a service the node calls, and one called by the application */
    const int echo = 3 + 2 * P_ECHO;
    v = nat16(1, 406);
    CHECK(cyphal_reg_write(&s, (size_t)echo, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 101);
    CHECK(cyphal_reg_write(&s, (size_t)echo, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 430);     /* but not one the node serves */
    CHECK(cyphal_reg_write(&s, (size_t)echo, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);

    /* A table whose called services clash with each other, or with one the node calls, is refused */
    cyphal_port_t clash[PANEL_N];
    memcpy(clash, PANEL, sizeof(clash));
    clash[P_LIGHT].default_id = 100;
    CHECK(!cyphal_reg_init(clash, PANEL_N, "r2.node.name", NULL, 0, why, sizeof(why)) &&
          strstr(why, "service 100 is uavcan.cln.motion.id's") != NULL);
    memcpy(clash, PANEL, sizeof(clash));
    clash[P_LIGHT].default_id = 430;
    CHECK(!cyphal_reg_init(clash, PANEL_N, "r2.node.name", NULL, 0, why, sizeof(why)) &&
          strstr(why, "one of the node's own ports") != NULL);
    memcpy(clash, PANEL, sizeof(clash));
    clash[P_LIGHT].default_id = 600;
    CHECK(!cyphal_reg_init(clash, PANEL_N, "r2.node.name", NULL, 0, why, sizeof(why)) &&
          strstr(why, "is no service-ID") != NULL);
    /* Saved IDs that clash (a backup from elsewhere): the later port called turned off */
    CHECK(cyphal_reg_init(PANEL, PANEL_N, "r2.node.name", NULL, 0, why, sizeof(why)));
    cyphal_ports_default(&s);
    s.port_id[P_LIGHT] = 100;
    CHECK(cyphal_ports_settle(&s) == (1u << P_LIGHT) && s.port_id[P_LIGHT] == CYPHAL_PORT_OFF);
    s.port_id[P_LIGHT] = 430;
    CHECK(cyphal_ports_settle(&s) == (1u << P_LIGHT));
    s.port_id[P_LIGHT] = 385;
    CHECK(cyphal_ports_settle(&s) == 0);
    cyphal_reg_fixed(NULL, 0);
    cyphal_reg_called(NULL);
}

/* A standard service added to a serialized port list's clients (its first call in a run): read back, only that bit
 * changed; refused for a list that isn't one, or an ID that isn't a service-ID */
static void test_port_list(void)
{
    static uavcan_node_port_List_1_0 m, back;
    static uint8_t buf[uavcan_node_port_List_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_], want[sizeof(buf)];
    memset(&m, 0, sizeof(m));
    uavcan_node_port_SubjectIDList_1_0_select_sparse_list_(&m.publishers);
    uavcan_node_port_SubjectIDList_1_0_select_sparse_list_(&m.subscribers);
    const uint16_t pubs[] = { 7509, 7510, 2303 };
    for (size_t i = 0; i < 3; i++) {
        m.publishers.sparse_list.elements[m.publishers.sparse_list.count++].value = pubs[i];
    }
    m.subscribers.sparse_list.elements[m.subscribers.sparse_list.count++].value = 2300;
    m.clients.mask_bitpacked_[MOTION / 8] |= (uint8_t)(1u << (MOTION % 8));
    m.servers.mask_bitpacked_[430 / 8] |= (uint8_t)(1u << (430 % 8));
    size_t size = sizeof(buf);
    CHECK(uavcan_node_port_List_1_0_serialize_(&m, buf, &size) == 0 && size > 4 + 6 + 4 + 2 + 2 * (4 + 64));
    /* What the serializer makes of the list with 430 and 511 among the clients too */
    m.clients.mask_bitpacked_[430 / 8] |= (uint8_t)(1u << (430 % 8));
    m.clients.mask_bitpacked_[511 / 8] |= (uint8_t)(1u << (511 % 8));
    size_t want_size = sizeof(want);
    CHECK(uavcan_node_port_List_1_0_serialize_(&m, want, &want_size) == 0 && want_size == size);
    CHECK(cyphal_port_list_add_client(buf, size, 430) && cyphal_port_list_add_client(buf, size, 511));
    CHECK(memcmp(buf, want, size) == 0);
    size_t n = size;
    CHECK(uavcan_node_port_List_1_0_deserialize_(&back, buf, &n) == 0 && n == size);
    CHECK(back.clients.mask_bitpacked_[430 / 8] & (1u << (430 % 8)) && back.servers.mask_bitpacked_[430 / 8]);
    CHECK(cyphal_port_list_add_client(buf, size, 430) && memcmp(buf, want, size) == 0);     /* twice: the same */
    /* Refused, the list untouched: not a service-ID; cut short, anywhere up to the servers' mask */
    CHECK(!cyphal_port_list_add_client(buf, size, 512));
    for (size_t cut = 0; cut < size - 4 - 64; cut++) {
        CHECK(!cyphal_port_list_add_client(buf, cut, 100));
    }
    CHECK(memcmp(buf, want, size) == 0);
    /* A delimiter header that says more than the list holds */
    buf[0] = 0xFF;
    CHECK(!cyphal_port_list_add_client(buf, size, 100));
}

/* A heartbeat read from its frame (cyphal_heartbeat_frame(), cyphal_app_t.on_heartbeat): the frames libcanard makes of
 * one, and frames that aren't one */
static void heartbeat_push(node_t *n, uint16_t subject, enum CanardTransferKind kind, const void *data, size_t size,
                           uint8_t tid)
{
    const struct CanardTransferMetadata meta = { CanardPriorityNominal, kind, subject,
                                                 kind == CanardTransferKindMessage ? CANARD_NODE_ID_UNSET : HOLO_ID, tid };
    CHECK(canardTxPush(&n->txq, &n->ins, S, &meta, (struct CanardPayload){ size, data }, 0, NULL) >= 1);
}

static bool heartbeat_read(node_t *n, uint8_t *from, cyphal_heartbeat_t *hb)
{
    const struct CanardTxQueueItem *item = canardTxPeek(&n->txq);
    CHECK(item != NULL);
    const bool ok = cyphal_heartbeat_frame((uint32_t)item->frame.extended_can_id, true, false, item->frame.payload.data,
                                           (uint8_t)item->frame.payload.size, from, hb);
    discard(n);
    return ok;
}

static void test_heartbeat(void)
{
    node_t holo;
    node_init(&holo, HOLO_ID, CanardTransferKindRequest, NULL, 0, 0);
    uint8_t buf[uavcan_node_Heartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    const uavcan_node_Heartbeat_1_0 hb = { .uptime = 0x01020304u, .health = { 1 }, .mode = { 2 },
                                           .vendor_specific_status_code = 0xA5 };
    size_t size = sizeof(buf);
    CHECK(uavcan_node_Heartbeat_1_0_serialize_(&hb, buf, &size) == 0 && size == 7);
    uint8_t from = 0;
    cyphal_heartbeat_t got = { 0 };
    for (uint8_t tid = 0; tid < 3; tid++) {         /* any transfer-ID: the toggle bit starts at 1 in each */
        heartbeat_push(&holo, uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, CanardTransferKindMessage, buf, size, tid);
        CHECK(heartbeat_read(&holo, &from, &got));
        CHECK(from == HOLO_ID && got.uptime_s == 0x01020304u && got.health == 1 && got.mode == 2 && got.vssc == 0xA5);
    }
    const uavcan_node_Heartbeat_1_0 zero = { 0 };
    size = sizeof(buf);
    CHECK(uavcan_node_Heartbeat_1_0_serialize_(&zero, buf, &size) == 0);
    heartbeat_push(&holo, uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, CanardTransferKindMessage, buf, size, 7);
    CHECK(heartbeat_read(&holo, &from, &got) && got.uptime_s == 0 && got.health == 0 && got.mode == 0 && got.vssc == 0);

    /* Not a heartbeat, and *from and *hb untouched: another subject, a request on 7509's number, a transfer of more than
     * one frame, the same frame shorter or anonymous, an 11-bit or remote frame */
    from = 77;
    got.uptime_s = 77;
    heartbeat_push(&holo, 7510, CanardTransferKindMessage, buf, 7, 0);
    CHECK(!heartbeat_read(&holo, &from, &got));
    heartbeat_push(&holo, 430, CanardTransferKindRequest, buf, 7, 0);   /* a request, not on 7509's number */
    CHECK(!heartbeat_read(&holo, &from, &got));
    /* A service frame whose bits read as subject 7509: a request on service 117 to node 42 from node 100. A subject
     * field is bits 8..20, here (117 << 6) | (42 >> 1) = 7509; only the service bit (25) tells it apart */
    {
        const uint32_t alias = (4u << 26) | (1u << 25) | (1u << 24) | (117u << 14) | (42u << 7) | 100u;
        const uint8_t data[8] = { 1, 0, 0, 0, 0, 0, 0, 0xE0 };
        CHECK(((alias >> 8) & 0x1FFF) == uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_);
        CHECK(!cyphal_heartbeat_frame(alias, true, false, data, 8, &from, &got));
    }
    uint8_t long_buf[20] = { 0 };
    heartbeat_push(&holo, uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, CanardTransferKindMessage, long_buf, sizeof(long_buf), 0);
    CHECK(!heartbeat_read(&holo, &from, &got));     /* the first of three frames: no end of transfer */
    discard(&holo);
    heartbeat_push(&holo, uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, CanardTransferKindMessage, buf, 6, 0);
    CHECK(!heartbeat_read(&holo, &from, &got));     /* 6 bytes and a tail */
    CHECK(from == 77 && got.uptime_s == 77);
    const uint8_t frame[8] = { 1, 0, 0, 0, 0, 0, 0, 0xE0 };
    const uint32_t id = (4u << 26) | ((uint32_t)uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_ << 8) | HOLO_ID;
    CHECK(cyphal_heartbeat_frame(id, true, false, frame, 8, &from, &got) && from == HOLO_ID && got.uptime_s == 1);
    from = 77;
    CHECK(!cyphal_heartbeat_frame(id | (1u << 24), true, false, frame, 8, &from, &got));    /* anonymous */
    CHECK(!cyphal_heartbeat_frame(id, false, false, frame, 8, &from, &got));                /* 11-bit */
    CHECK(!cyphal_heartbeat_frame(id, true, true, frame, 8, &from, &got));                  /* remote */
    const uint8_t middle[8] = { 1, 0, 0, 0, 0, 0, 0, 0x40 };                               /* end, no start */
    CHECK(!cyphal_heartbeat_frame(id, true, false, middle, 8, &from, &got) && from == 77);
}

int main(void)
{
    test_pool();
    test_none_held();
    test_answered();
    test_timeout_and_late();
    test_wrong_source();
    test_restart();
    test_tids();
    test_tid_held();
    test_generation();
    test_order();
    test_tx_deadline();
    test_registers();
    test_port_list();
    test_heartbeat();
    printf("cyphal_client: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
