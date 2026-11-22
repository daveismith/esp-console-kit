/*
 * See cyphal_alloc.h: the allocator's plain parts. Plain C only: tests/host builds this file on the computer.
 * Built into the component only with CONFIG_CYPHAL_PNP_ALLOCATOR (CMakeLists.txt), so that without it the
 * library is what it was.
 */
#include <string.h>
#include "cyphal_alloc.h"
#include "cyphal_util.h"
#include "uavcan/pnp/NodeIDAllocationData_1_0.h"

_Static_assert(CYPHAL_ALLOC_SUBJECT == uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_, "subject 8166");
_Static_assert(CYPHAL_ALLOC_REPLY_SIZE == uavcan_pnp_NodeIDAllocationData_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_,
               "the answer is the type's largest form");

void cyphal_alloc_table_init(cyphal_alloc_table_t *t, cyphal_alloc_row_t *rows, size_t capacity)
{
    t->rows = rows;
    t->capacity = rows != NULL ? capacity : 0;
    t->count = 0;
}

void cyphal_alloc_table_clear(cyphal_alloc_table_t *t)
{
    t->count = 0;
}

int cyphal_alloc_table_find(const cyphal_alloc_table_t *t, uint64_t pseudo_id)
{
    pseudo_id &= CYPHAL_PSEUDO_ID_MASK;
    for (size_t i = 0; i < t->count; i++) {
        if (!(t->rows[i].flags & CYPHAL_ALLOC_ROW_HEARD) && t->rows[i].pseudo_id == pseudo_id) {
            return (int)i;
        }
    }
    return -1;
}

int cyphal_alloc_table_find_node(const cyphal_alloc_table_t *t, uint8_t node_id)
{
    for (size_t i = 0; i < t->count; i++) {
        if (t->rows[i].node_id == node_id) {
            return (int)i;
        }
    }
    return -1;
}

bool cyphal_alloc_table_forget(cyphal_alloc_table_t *t, uint8_t node_id)
{
    const int i = cyphal_alloc_table_find_node(t, node_id);
    if (i < 0) {
        return false;
    }
    memmove(&t->rows[i], &t->rows[i + 1], (t->count - (size_t)i - 1) * sizeof(t->rows[0]));
    t->count--;
    return true;
}

int cyphal_alloc_pick(const cyphal_alloc_table_t *t, uint8_t first, uint8_t last, uint8_t own,
                      cyphal_alloc_busy_fn busy, void *ctx)
{
    /* The rows' node-IDs and our own as one bitmap, in one pass: then each candidate is a bit test, not a scan */
    uint32_t taken[(CYPHAL_NODE_ID_MAX + 1) / 32] = { 0 };
    for (size_t i = 0; i < t->count; i++) {
        taken[t->rows[i].node_id / 32] |= 1u << (t->rows[i].node_id % 32);
    }
    if (own <= CYPHAL_NODE_ID_MAX) {
        taken[own / 32] |= 1u << (own % 32);
    }
    for (unsigned id = first; id <= last && id <= CYPHAL_NODE_ID_MAX; id++) {
        if (!(taken[id / 32] & (1u << (id % 32))) && (busy == NULL || !busy(ctx, (uint8_t)id))) {
            return (int)id;
        }
    }
    return -1;
}

cyphal_alloc_result_t cyphal_alloc_serve(cyphal_alloc_table_t *t, uint64_t pseudo_id, uint8_t first, uint8_t last,
                                         uint8_t own, cyphal_alloc_busy_fn busy, void *ctx, uint8_t *node_id)
{
    pseudo_id &= CYPHAL_PSEUDO_ID_MASK;
    const int row = cyphal_alloc_table_find(t, pseudo_id);
    if (row >= 0) {
        *node_id = t->rows[row].node_id;
        return *node_id == own ? CYPHAL_ALLOC_OWN : CYPHAL_ALLOC_KNOWN;
    }
    if (t->count >= t->capacity) {
        return CYPHAL_ALLOC_FULL;
    }
    const int id = cyphal_alloc_pick(t, first, last, own, busy, ctx);
    if (id < 0) {
        return CYPHAL_ALLOC_NO_ID;
    }
    t->rows[t->count++] = (cyphal_alloc_row_t){ .pseudo_id = pseudo_id, .node_id = (uint8_t)id, .flags = 0 };
    *node_id = (uint8_t)id;
    return CYPHAL_ALLOC_NEW;
}

bool cyphal_alloc_heard(cyphal_alloc_table_t *t, uint8_t node_id, uint8_t first, uint8_t last, uint8_t own)
{
    if (node_id < first || node_id > last || node_id == own || node_id > CYPHAL_NODE_ID_MAX ||
        t->count >= t->capacity || cyphal_alloc_table_find_node(t, node_id) >= 0) {
        return false;
    }
    t->rows[t->count++] = (cyphal_alloc_row_t){ .pseudo_id = 0, .node_id = node_id, .flags = CYPHAL_ALLOC_ROW_HEARD };
    return true;
}

/* Zeroed, not _initialize_()d: see serialize_getinfo() in cyphal.c */
size_t cyphal_alloc_reply(uint8_t *out, size_t size, uint64_t pseudo_id, uint8_t node_id)
{
    if (node_id > CYPHAL_NODE_ID_MAX || size < CYPHAL_ALLOC_REPLY_SIZE) {
        return 0;
    }
    uavcan_pnp_NodeIDAllocationData_1_0 m;
    memset(&m, 0, sizeof(m));
    m.unique_id_hash = pseudo_id & CYPHAL_PSEUDO_ID_MASK;
    m.allocated_node_id.count = 1;
    m.allocated_node_id.elements[0].value = node_id;
    return uavcan_pnp_NodeIDAllocationData_1_0_serialize_(&m, out, &size) < 0 ? 0 : size;
}

bool cyphal_alloc_request(const void *payload, size_t size, bool anonymous, uint64_t *pseudo_id)
{
    uavcan_pnp_NodeIDAllocationData_1_0 m;
    memset(&m, 0, sizeof(m));
    size_t n = size;
    /* Its bytes must all be there: the deserializer reads a short one as zeros past the end, a pseudo-ID that
     * nobody sent */
    if (!anonymous || size < CYPHAL_ALLOC_REQUEST_SIZE ||
        uavcan_pnp_NodeIDAllocationData_1_0_deserialize_(&m, (const uint8_t *)payload, &n) < 0 ||
        m.allocated_node_id.count != 0) {
        return false;
    }
    *pseudo_id = m.unique_id_hash & CYPHAL_PSEUDO_ID_MASK;
    return true;
}

bool cyphal_alloc_other_allocator(uint32_t can_id, bool ext, bool rtr, const uint8_t *data, uint8_t len, uint8_t own,
                                  uint8_t *source)
{
    /* cyphal_frame_source(): a message from a regular source (anonymous ones, requests, have none) */
    const int from = cyphal_frame_source(can_id, ext, rtr, len);
    if (from < 0 || (can_id & (1u << 25)) || ((can_id >> 8) & 0x1FFF) != CYPHAL_ALLOC_SUBJECT || from == own ||
        !(data[len - 1] & 0x80)) {      /* the tail byte's start of transfer */
        return false;
    }
    *source = (uint8_t)from;
    return true;
}

static void put48(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 6; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get48(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 5; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

size_t cyphal_alloc_encode(const cyphal_alloc_table_t *t, uint8_t *out, size_t size)
{
    const size_t need = CYPHAL_ALLOC_BLOB_SIZE(t->count);
    if (size < need || t->count > 255) {
        return 0;
    }
    out[0] = CYPHAL_ALLOC_BLOB_VERSION;
    out[1] = (uint8_t)t->count;
    for (size_t i = 0; i < t->count; i++) {
        uint8_t *p = out + CYPHAL_ALLOC_BLOB_SIZE(i);
        put48(p, t->rows[i].pseudo_id);
        p[6] = t->rows[i].node_id;
        p[7] = t->rows[i].flags;
    }
    return need;
}

static cyphal_alloc_row_t blob_row(const uint8_t *data, size_t i)
{
    const uint8_t *p = data + CYPHAL_ALLOC_BLOB_SIZE(i);
    return (cyphal_alloc_row_t){ .pseudo_id = get48(p), .node_id = p[6], .flags = p[7] };
}

cyphal_alloc_blob_t cyphal_alloc_decode(cyphal_alloc_table_t *t, const uint8_t *data, size_t size, size_t *dropped)
{
    cyphal_alloc_table_clear(t);
    *dropped = 0;
    if (size >= 1 && data[0] > CYPHAL_ALLOC_BLOB_VERSION) {
        return CYPHAL_ALLOC_BLOB_NEWER;
    }
    if (size < 2 || data[0] != CYPHAL_ALLOC_BLOB_VERSION || size != CYPHAL_ALLOC_BLOB_SIZE(data[1])) {
        return CYPHAL_ALLOC_BLOB_BAD;
    }
    const size_t rows = data[1];
    /* Checked whole first (a bad row refuses the blob), then kept given rows first, heard rows after, while there
     * is room: a heard row is made again by hearing its node, a given one only by the node asking */
    for (size_t i = 0; i < rows; i++) {
        const cyphal_alloc_row_t r = blob_row(data, i);
        if (r.node_id > CYPHAL_NODE_ID_MAX || (r.flags & ~CYPHAL_ALLOC_ROW_HEARD)) {
            return CYPHAL_ALLOC_BLOB_BAD;
        }
        for (size_t j = 0; j < i; j++) {
            const cyphal_alloc_row_t q = blob_row(data, j);
            if (q.node_id == r.node_id ||
                (!(r.flags & CYPHAL_ALLOC_ROW_HEARD) && !(q.flags & CYPHAL_ALLOC_ROW_HEARD) &&
                 q.pseudo_id == r.pseudo_id)) {
                return CYPHAL_ALLOC_BLOB_BAD;
            }
        }
    }
    for (int heard = 0; heard < 2; heard++) {
        for (size_t i = 0; i < rows; i++) {
            const cyphal_alloc_row_t r = blob_row(data, i);
            if (((r.flags & CYPHAL_ALLOC_ROW_HEARD) != 0) != heard) {
                continue;
            }
            if (t->count < t->capacity) {
                t->rows[t->count++] = r;
            } else {
                (*dropped)++;
            }
        }
    }
    return *dropped > 0 ? CYPHAL_ALLOC_BLOB_TRUNCATED : CYPHAL_ALLOC_BLOB_OK;
}

void cyphal_alloc_recent_note(cyphal_alloc_recent_t *r, uint8_t node_id, int64_t now_us)
{
    size_t oldest = 0;
    for (size_t i = 0; i < CYPHAL_ALLOC_RECENT; i++) {
        if (r->node_id[i] == node_id) {
            oldest = i;
            break;
        }
        if (r->at_us[i] < r->at_us[oldest]) {
            oldest = i;
        }
    }
    r->node_id[oldest] = node_id;
    r->at_us[oldest] = now_us;
}

bool cyphal_alloc_recent_has(const cyphal_alloc_recent_t *r, uint8_t node_id, int64_t now_us)
{
    for (size_t i = 0; i < CYPHAL_ALLOC_RECENT; i++) {
        if (r->node_id[i] == node_id && r->at_us[i] != INT64_MIN && now_us - r->at_us[i] < CYPHAL_ALLOC_RECENT_US) {
            return true;
        }
    }
    return false;
}

void cyphal_alloc_recent_clear(cyphal_alloc_recent_t *r)
{
    for (size_t i = 0; i < CYPHAL_ALLOC_RECENT; i++) {
        r->node_id[i] = CYPHAL_NODE_ID_AUTO;
        r->at_us[i] = INT64_MIN;
    }
}
