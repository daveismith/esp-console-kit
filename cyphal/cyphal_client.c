/*
 * See cyphal_client.h: the client's plain parts. Plain C only: tests/host builds this file on the computer.
 * Built into the component only with CONFIG_CYPHAL_CLIENT (CMakeLists.txt), so that without it the library is
 * what it was.
 */
#include <string.h>
#include "canard.h"
#include "cyphal_client.h"
#include "cyphal_registers.h"
#include "uavcan/_register/Access_1_0.h"
#include "uavcan/node/GetInfo_1_0.h"
#include "uavcan/node/Heartbeat_1_0.h"

_Static_assert(CYPHAL_CALL_REQUEST_MAX == uavcan_register_Access_Request_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_,
               "a request: the largest the kit has a type for");
_Static_assert(CYPHAL_CALL_RESPONSE_MAX == uavcan_node_GetInfo_Response_1_0_EXTENT_BYTES_,
               "a response: the largest extent of the standard services called");
_Static_assert(CYPHAL_CALL_SERVERS == CYPHAL_NODE_ID_MAX + 1, "a transfer-ID for each node-ID");

void cyphal_call_counts_clear(cyphal_call_pool_t *p)
{
    p->counts = (cyphal_call_counts_t){ 0 };
}

void cyphal_call_pool_init(cyphal_call_pool_t *p, cyphal_call_t *calls, uint32_t *states, size_t count, uint8_t *tids,
                           size_t ports)
{
    p->calls = calls;
    p->states = states;
    p->count = calls != NULL && states != NULL ? count : 0;
    __atomic_store_n(&p->held, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&p->next_seq, 0, __ATOMIC_RELAXED);
    p->tids = tids;
    p->ports = tids != NULL ? ports : 0;
    for (size_t i = 0; i < p->count; i++) {
        __atomic_store_n(&states[i], CYPHAL_CALL_FREE, __ATOMIC_RELAXED);
    }
    if (p->tids != NULL) {
        memset(p->tids, 0, ports * CYPHAL_CALL_SERVERS);
    }
    cyphal_call_counts_clear(p);
}

/* A state word: the generation of `word` (its bits above the state's) with state s */
#define GEN_MASK        (~((1u << CYPHAL_CALL_STATE_BITS) - 1))
#define WORD(word, s)   (((word) & GEN_MASK) | (uint32_t)(s))

/* The record's state word */
static uint32_t word(const cyphal_call_pool_t *p, size_t i)
{
    return __atomic_load_n(&p->states[i], __ATOMIC_ACQUIRE);
}

static cyphal_call_state_t state(const cyphal_call_pool_t *p, size_t i)
{
    return CYPHAL_CALL_STATE(word(p, i));
}

/* Released, its generation kept (only its holder sets a record's state): what was written into the record before is
 * seen by whoever acquires the new state */
static void set(cyphal_call_pool_t *p, size_t i, cyphal_call_state_t s)
{
    __atomic_store_n(&p->states[i], WORD(__atomic_load_n(&p->states[i], __ATOMIC_RELAXED), s), __ATOMIC_RELEASE);
}

/* from -> to if the record's word is still `from`: whether it was */
static bool swap(cyphal_call_pool_t *p, size_t i, uint32_t from, uint32_t to)
{
    return __atomic_compare_exchange_n(&p->states[i], &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* The record's sequence number: written by its caller before QUEUED is released, read by the node's task and the
 * worker, which may look at a record its caller is filling again (and then don't use what they read) */
static uint32_t seq(const cyphal_call_pool_t *p, size_t i)
{
    return __atomic_load_n(&p->calls[i].seq, __ATOMIC_RELAXED);
}

/* Of the records in state s, the one with the lowest sequence number (wrapping): its index and word, -1 for none */
static int oldest(const cyphal_call_pool_t *p, cyphal_call_state_t s, uint32_t *ticket)
{
    int best = -1;
    uint32_t best_seq = 0, best_word = 0;
    for (size_t i = 0; i < p->count; i++) {
        const uint32_t w = word(p, i);
        if (CYPHAL_CALL_STATE(w) != s) {
            continue;
        }
        const uint32_t n = seq(p, i);
        if (best < 0 || (int32_t)(n - best_seq) < 0) {
            best = (int)i;
            best_seq = n;
            best_word = w;
        }
    }
    if (best >= 0 && ticket != NULL) {
        *ticket = best_word;
    }
    return best;
}

/* Whether no record is held: then none is queued, sent or done, and a look at them can stop here. `held` counts a
 * record from before it leaves FREE (or just after, while only FILLING) until after it is FREE again */
static bool none_held(const cyphal_call_pool_t *p)
{
    return __atomic_load_n(&p->held, __ATOMIC_ACQUIRE) == 0;
}

/* A record FREE again: no longer held */
static void freed(cyphal_call_pool_t *p)
{
    __atomic_sub_fetch(&p->held, 1, __ATOMIC_RELEASE);
}

int cyphal_call_take(cyphal_call_pool_t *p)
{
    for (size_t i = 0; i < p->count; i++) {
        const uint32_t w = word(p, i);
        /* Its generation counted on: a ticket of the call before never names this one */
        if (CYPHAL_CALL_STATE(w) == CYPHAL_CALL_FREE &&
            swap(p, i, w, WORD(w + (1u << CYPHAL_CALL_STATE_BITS), CYPHAL_CALL_FILLING))) {
            __atomic_add_fetch(&p->held, 1, __ATOMIC_RELAXED);     /* seen before QUEUED is (its release) */
            return (int)i;
        }
    }
    return -1;
}

/* Then a full fence: a caller that queues and then looks at the node's state, against a stop that changes the state
 * and then fails every call queued (cyphal_call_fail_all(), which fences first), the one or the other sees it */
uint32_t cyphal_call_queue(cyphal_call_pool_t *p, size_t i)
{
    __atomic_store_n(&p->calls[i].seq, __atomic_fetch_add(&p->next_seq, 1, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    const uint32_t ticket = WORD(__atomic_load_n(&p->states[i], __ATOMIC_RELAXED), CYPHAL_CALL_QUEUED);
    __atomic_store_n(&p->states[i], ticket, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return ticket;
}

bool cyphal_call_cancel(cyphal_call_pool_t *p, size_t i, uint32_t ticket)
{
    if (CYPHAL_CALL_STATE(ticket) != CYPHAL_CALL_QUEUED || !swap(p, i, ticket, WORD(ticket, CYPHAL_CALL_FREE))) {
        return false;
    }
    freed(p);
    return true;
}

int cyphal_call_next_queued(const cyphal_call_pool_t *p, uint32_t *ticket)
{
    return none_held(p) ? -1 : oldest(p, CYPHAL_CALL_QUEUED, ticket);
}

bool cyphal_call_waiting(const cyphal_call_pool_t *p)
{
    if (none_held(p)) {
        return false;
    }
    for (size_t i = 0; i < p->count; i++) {
        if (state(p, i) == CYPHAL_CALL_QUEUED) {
            return true;
        }
    }
    return false;
}

bool cyphal_call_begin(cyphal_call_pool_t *p, size_t i, uint32_t ticket)
{
    if (CYPHAL_CALL_STATE(ticket) != CYPHAL_CALL_QUEUED || !swap(p, i, ticket, WORD(ticket, CYPHAL_CALL_SENDING))) {
        return false;
    }
    p->counts.made++;
    return true;
}

void cyphal_call_hold(cyphal_call_pool_t *p, size_t i)
{
    p->counts.made--;
    set(p, i, CYPHAL_CALL_QUEUED);
}

/* Whether a call awaiting its answer from `server` on service_id holds transfer-ID tid */
static bool tid_held(const cyphal_call_pool_t *p, uint8_t server, uint16_t service_id, uint8_t tid)
{
    for (size_t i = 0; i < p->count; i++) {
        const cyphal_call_t *c = &p->calls[i];
        if (state(p, i) == CYPHAL_CALL_SENT && c->server == server && c->service_id == service_id &&
            c->transfer_id == tid) {
            return true;
        }
    }
    return false;
}

uint8_t cyphal_call_next_tid(cyphal_call_pool_t *p, size_t port, uint8_t server, uint16_t service_id)
{
    if (port >= p->ports || server >= CYPHAL_CALL_SERVERS) {
        return 0;
    }
    uint8_t *t = &p->tids[port * CYPHAL_CALL_SERVERS + server];
    uint8_t tid = *t;
    /* SENT calls are the node task's, as this is: none changes while it looks. The call being sent is SENDING, so
     * with 32 records at most a lap finds one free; with more, the lap's end is taken regardless */
    for (unsigned k = 0; k < CANARD_TRANSFER_ID_MAX && tid_held(p, server, service_id, tid); k++) {
        tid = (uint8_t)((tid + 1) % (CANARD_TRANSFER_ID_MAX + 1));
    }
    *t = (uint8_t)((tid + 1) % (CANARD_TRANSFER_ID_MAX + 1));
    return tid;
}

int64_t cyphal_call_tx_deadline(const cyphal_call_pool_t *p, size_t i, int64_t now_us, int64_t most_us)
{
    const int64_t before = p->calls[i].deadline_us - 1;
    return before < now_us + most_us ? before : now_us + most_us;
}

void cyphal_call_sent(cyphal_call_pool_t *p, size_t i, uint16_t service_id, uint8_t transfer_id)
{
    p->calls[i].service_id = service_id;
    p->calls[i].transfer_id = transfer_id;
    set(p, i, CYPHAL_CALL_SENT);
}

void cyphal_call_finish(cyphal_call_pool_t *p, size_t i, cyphal_call_outcome_t outcome)
{
    cyphal_call_t *c = &p->calls[i];
    c->outcome = outcome;
    if (outcome != CYPHAL_CALL_ANSWERED) {
        c->response_size = 0;
    }
    switch (outcome) {
    case CYPHAL_CALL_ANSWERED:  p->counts.answered++; break;
    case CYPHAL_CALL_TIMED_OUT: p->counts.timed_out++; break;
    case CYPHAL_CALL_NOT_SENT:  p->counts.not_sent++; break;
    default:                    p->counts.failed++; break;
    }
    set(p, i, CYPHAL_CALL_DONE);
}

int cyphal_call_match(cyphal_call_pool_t *p, uint16_t service_id, uint8_t source, uint8_t transfer_id,
                      const void *payload, size_t size)
{
    const size_t count = none_held(p) ? 0 : p->count;
    for (size_t i = 0; i < count; i++) {
        cyphal_call_t *c = &p->calls[i];
        if (state(p, i) == CYPHAL_CALL_SENT && c->service_id == service_id && c->server == source &&
            c->transfer_id == transfer_id) {
            c->response_size = size < sizeof(c->response) ? size : sizeof(c->response);
            if (c->response_size > 0) {
                memcpy(c->response, payload, c->response_size);
            }
            cyphal_call_finish(p, i, CYPHAL_CALL_ANSWERED);
            return (int)i;
        }
    }
    p->counts.dropped++;
    return -1;
}

size_t cyphal_call_expire(cyphal_call_pool_t *p, int64_t now_us)
{
    size_t n = 0;
    if (none_held(p)) {
        return 0;
    }
    for (size_t i = 0; i < p->count; i++) {
        const uint32_t w = word(p, i);
        const cyphal_call_state_t s = CYPHAL_CALL_STATE(w);
        if ((s != CYPHAL_CALL_QUEUED && s != CYPHAL_CALL_SENT) || p->calls[i].deadline_us > now_us) {
            continue;
        }
        /* A queued one is taken first, its caller may be cancelling it: the begin fails if the record has changed
         * hands since its word was read, and the deadline read with it isn't used. A sent one is the node task's */
        if (s == CYPHAL_CALL_SENT || cyphal_call_begin(p, i, w)) {
            cyphal_call_finish(p, i, CYPHAL_CALL_TIMED_OUT);
            n++;
        }
    }
    return n;
}

size_t cyphal_call_fail_all(cyphal_call_pool_t *p)
{
    /* The other half of cyphal_call_queue()'s order: the node's new state is in memory before `held` and the states
     * are read (a caller's take counted `held` before its fence) */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    size_t n = 0;
    if (none_held(p)) {
        return 0;
    }
    for (size_t i = 0; i < p->count; i++) {
        const uint32_t w = word(p, i);
        const cyphal_call_state_t s = CYPHAL_CALL_STATE(w);
        if (s == CYPHAL_CALL_SENDING || s == CYPHAL_CALL_SENT ||
            (s == CYPHAL_CALL_QUEUED && cyphal_call_begin(p, i, w))) {
            cyphal_call_finish(p, i, CYPHAL_CALL_FAILED);
            n++;
        }
    }
    return n;
}

int64_t cyphal_call_next_deadline(const cyphal_call_pool_t *p)
{
    int64_t next = INT64_MAX;
    if (none_held(p)) {
        return next;
    }
    for (size_t i = 0; i < p->count; i++) {
        const cyphal_call_state_t s = state(p, i);
        if ((s == CYPHAL_CALL_QUEUED || s == CYPHAL_CALL_SENT) && p->calls[i].deadline_us < next) {
            next = p->calls[i].deadline_us;
        }
    }
    return next;
}

int cyphal_call_done(const cyphal_call_pool_t *p)
{
    return none_held(p) ? -1 : oldest(p, CYPHAL_CALL_DONE, NULL);
}

void cyphal_call_free(cyphal_call_pool_t *p, size_t i)
{
    set(p, i, CYPHAL_CALL_FREE);
    freed(p);
}

size_t cyphal_call_held(const cyphal_call_pool_t *p)
{
    return __atomic_load_n(&p->held, __ATOMIC_ACQUIRE);
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* The list as uavcan.node.port.List.1.0 serializes: the publishers and the subscribers (each a 32-bit delimiter
 * header, little-endian, and as many bytes as it says), then the clients (a header, 64, and their 512-bit mask, bit
 * n for service-ID n, the lowest first), then the servers */
bool cyphal_port_list_add_client(uint8_t *list, size_t size, uint16_t service_id)
{
    size_t at = 0;
    for (int k = 0; k < 2; k++) {
        if (size - at < 4 || get_u32(&list[at]) > size - at - 4) {
            return false;
        }
        at += 4 + get_u32(&list[at]);
    }
    if (service_id > CYPHAL_SERVICE_ID_MAX || size - at < 4 + 64 || get_u32(&list[at]) != 64) {
        return false;
    }
    list[at + 4 + service_id / 8] |= (uint8_t)(1u << (service_id % 8));
    return true;
}

bool cyphal_heartbeat_frame(uint32_t can_id, bool ext, bool rtr, const uint8_t *data, uint8_t len, uint8_t *source,
                            cyphal_heartbeat_t *out)
{
    /* cyphal_frame_source(): a regular source (not anonymous), an extended ID, no remote frame, some data */
    const int from = cyphal_frame_source(can_id, ext, rtr, len);
    enum { TAIL_SINGLE = 0xE0 };    /* the tail byte's start of transfer, end of transfer and toggle: one frame */
    if (from < 0 || (can_id & (1u << 25)) != 0 || ((can_id >> 8) & 0x1FFF) != uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_ ||
        len != uavcan_node_Heartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_ + 1 || (data[len - 1] & TAIL_SINGLE) != TAIL_SINGLE) {
        return false;
    }
    uavcan_node_Heartbeat_1_0 hb;
    memset(&hb, 0, sizeof(hb));
    size_t size = (size_t)len - 1;
    if (uavcan_node_Heartbeat_1_0_deserialize_(&hb, data, &size) < 0) {
        return false;
    }
    *source = (uint8_t)from;
    *out = (cyphal_heartbeat_t){ .uptime_s = hb.uptime, .health = hb.health.value, .mode = hb.mode.value,
                                 .vssc = hb.vendor_specific_status_code };
    return true;
}
