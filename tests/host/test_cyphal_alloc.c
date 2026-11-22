/*
 * Host-built tests for cyphal/cyphal_alloc.c: the plug-and-play allocator's plain parts (the table, the
 * answer, a request read, another allocator's frame, the saved form). Built with cyphal_util.c, whose
 * allocatee side (cyphal_pnp_accept()) reads the answers back as a node would.
 */
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "cyphal_alloc.h"
#include "cyphal_util.h"

#define CAP 4

/* Node-IDs marked busy, for cyphal_alloc_pick() */
static bool busy_set(void *ctx, uint8_t node_id)
{
    const bool *busy = ctx;
    return busy[node_id];
}

static void test_table(void)
{
    cyphal_alloc_row_t rows[CAP];
    cyphal_alloc_table_t t;
    cyphal_alloc_table_init(&t, rows, CAP);
    bool busy[128] = { false };
    uint8_t id = 0;

    /* The lowest free in range, never our own: 100 is ours */
    CHECK(cyphal_alloc_serve(&t, 0xAAA, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_NEW && id == 101);
    /* A known pseudo-ID gets its own again, and no new row */
    CHECK(cyphal_alloc_serve(&t, 0xAAA, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_KNOWN && id == 101);
    CHECK(t.count == 1);
    /* Only the low 48 bits are the pseudo-ID */
    CHECK(cyphal_alloc_serve(&t, 0xFFFF000000000AAAull, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_KNOWN &&
          id == 101);
    /* Busy (heard lately) is skipped */
    busy[102] = true;
    CHECK(cyphal_alloc_serve(&t, 0xBBB, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_NEW && id == 103);
    /* Range used up (100 ours, 101 and 103 rows, 102 busy): no node-ID, no row */
    CHECK(cyphal_alloc_serve(&t, 0xCCC, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_NO_ID && t.count == 2);
    busy[102] = false;
    CHECK(cyphal_alloc_serve(&t, 0xCCC, 100, 103, 100, busy_set, busy, &id) == CYPHAL_ALLOC_NEW && id == 102);
    CHECK(cyphal_alloc_table_find(&t, 0xCCC) == 2 && cyphal_alloc_table_find_node(&t, 103) == 1);
    CHECK(cyphal_alloc_table_find(&t, 0xDDD) == -1 && cyphal_alloc_table_find_node(&t, 100) == -1);

    /* A wide range: the table fills first */
    CHECK(cyphal_alloc_serve(&t, 0xDDD, 100, 119, 100, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 104 &&
          t.count == CAP);
    CHECK(cyphal_alloc_serve(&t, 0xEEE, 100, 119, 100, NULL, NULL, &id) == CYPHAL_ALLOC_FULL && t.count == CAP);
    /* ... and the rows already there still answer */
    CHECK(cyphal_alloc_serve(&t, 0xAAA, 100, 119, 100, NULL, NULL, &id) == CYPHAL_ALLOC_KNOWN && id == 101);

    /* Forget: the ID is free again, the rest keep their order */
    CHECK(cyphal_alloc_table_forget(&t, 103) && t.count == 3);
    CHECK(!cyphal_alloc_table_forget(&t, 103));
    CHECK(t.rows[0].node_id == 101 && t.rows[1].node_id == 102 && t.rows[2].node_id == 104);
    CHECK(cyphal_alloc_serve(&t, 0xEEE, 100, 119, 100, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 103);
    /* Clear: from the range's first again */
    cyphal_alloc_table_clear(&t);
    CHECK(t.count == 0);
    CHECK(cyphal_alloc_serve(&t, 0xEEE, 100, 119, 99, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 100);

    /* A range past 127 stops at 127 */
    cyphal_alloc_table_clear(&t);
    CHECK(cyphal_alloc_pick(&t, 127, 255, 0, NULL, NULL) == 127);
    CHECK(cyphal_alloc_pick(&t, 127, 255, 127, NULL, NULL) == -1);
    /* A table with no memory is full */
    cyphal_alloc_table_t none;
    cyphal_alloc_table_init(&none, NULL, 8);
    CHECK(cyphal_alloc_serve(&none, 1, 100, 119, 0, NULL, NULL, &id) == CYPHAL_ALLOC_FULL);
}

static void test_heard(void)
{
    cyphal_alloc_row_t rows[CAP];
    cyphal_alloc_table_t t;
    cyphal_alloc_table_init(&t, rows, CAP);
    uint8_t id = 0;
    /* In range, not ours, no row: a heard row; again, nothing */
    CHECK(cyphal_alloc_heard(&t, 100, 100, 119, 52));
    CHECK(!cyphal_alloc_heard(&t, 100, 100, 119, 52));
    CHECK(t.count == 1 && (t.rows[0].flags & CYPHAL_ALLOC_ROW_HEARD) && t.rows[0].node_id == 100);
    /* Out of range, or our own: nothing */
    CHECK(!cyphal_alloc_heard(&t, 99, 100, 119, 52) && !cyphal_alloc_heard(&t, 120, 100, 119, 52));
    CHECK(!cyphal_alloc_heard(&t, 105, 100, 119, 105) && t.count == 1);
    /* A heard row is never given, and never matches a pseudo-ID, 0 included */
    CHECK(cyphal_alloc_serve(&t, 0, 100, 119, 52, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 101);
    CHECK(cyphal_alloc_table_find(&t, 0) == 1);
    /* A node given an ID here and then heard: its own row, no second */
    CHECK(!cyphal_alloc_heard(&t, 101, 100, 119, 52) && t.count == 2);
    /* Full: nothing */
    CHECK(cyphal_alloc_heard(&t, 110, 100, 119, 52) && cyphal_alloc_heard(&t, 111, 100, 119, 52));
    CHECK(!cyphal_alloc_heard(&t, 112, 100, 119, 52) && t.count == CAP);
}

static void test_reply(void)
{
    uint8_t buf[16];
    uint8_t id = 0;
    const uint64_t pseudo = 0x0b3af4648a21ull;
    CHECK(cyphal_alloc_reply(buf, sizeof(buf), pseudo, 100) == CYPHAL_ALLOC_REPLY_SIZE);
    /* The bytes: the hash little-endian, an array of one, the node-ID's 16 bits */
    const uint8_t want[] = { 0x21, 0x8a, 0x64, 0xf4, 0x3a, 0x0b, 0x01, 100, 0x00 };
    CHECK(memcmp(buf, want, sizeof(want)) == 0);
    /* As an allocatee reads it: its own, from a regular source */
    CHECK(cyphal_pnp_accept(buf, CYPHAL_ALLOC_REPLY_SIZE, false, pseudo, &id) == CYPHAL_PNP_ALLOCATED && id == 100);
    CHECK(cyphal_pnp_accept(buf, CYPHAL_ALLOC_REPLY_SIZE, false, pseudo + 1, &id) == CYPHAL_PNP_OTHER);
    /* Too small, or no node-ID a CAN node can have */
    CHECK(cyphal_alloc_reply(buf, 8, pseudo, 100) == 0);
    CHECK(cyphal_alloc_reply(buf, sizeof(buf), pseudo, 128) == 0);
    CHECK(cyphal_alloc_reply(buf, sizeof(buf), pseudo, 127) == CYPHAL_ALLOC_REPLY_SIZE && buf[7] == 127);
    /* It takes two classic frames */
    CHECK(cyphal_frames(CYPHAL_ALLOC_REPLY_SIZE) == 2);
}

static void test_request(void)
{
    uint8_t buf[16];
    uint64_t pseudo = 0;
    /* What an allocatee sends (cyphal_pnp_request()) is a request */
    const size_t n = cyphal_pnp_request(buf, sizeof(buf), 0x123456789abcull);
    CHECK(n == CYPHAL_ALLOC_REQUEST_SIZE);
    CHECK(cyphal_alloc_request(buf, n, true, &pseudo) && pseudo == 0x123456789abcull);
    /* Not from a regular source, not cut short, not with a node-ID (an answer) */
    CHECK(!cyphal_alloc_request(buf, n, false, &pseudo));
    CHECK(!cyphal_alloc_request(buf, n - 1, true, &pseudo));
    uint8_t reply[16];
    CHECK(cyphal_alloc_reply(reply, sizeof(reply), 0x42, 101) == CYPHAL_ALLOC_REPLY_SIZE);
    CHECK(!cyphal_alloc_request(reply, CYPHAL_ALLOC_REPLY_SIZE, true, &pseudo));
    /* An array longer than one doesn't deserialize */
    uint8_t bad[] = { 1, 2, 3, 4, 5, 6, 2 };
    CHECK(!cyphal_alloc_request(bad, sizeof(bad), true, &pseudo));
    /* Bytes past the end are ignored, as the type's extent allows */
    buf[7] = 0x55;
    CHECK(cyphal_alloc_request(buf, 8, true, &pseudo) && pseudo == 0x123456789abcull);
}

/* A message frame's CAN ID: priority 4, the two reserved bits set, subject, source (or anonymous) */
static uint32_t message_id(uint16_t subject, uint8_t source, bool anonymous)
{
    return (4u << 26) | (anonymous ? 1u << 24 : 0) | (3u << 21) | ((uint32_t)subject << 8) | source;
}

static void test_other_allocator(void)
{
    uint8_t who = 0;
    const uint8_t first[8] = { 0, 0, 0, 0, 0, 0, 1, 0xA0 };     /* start of transfer, toggle */
    const uint8_t last[3] = { 100, 0, 0x40 };                   /* end of transfer, not its start */
    CHECK(cyphal_alloc_other_allocator(message_id(8166, 43, false), true, false, first, 8, 52, &who) && who == 43);
    /* Only the first frame of its transfer counts (one answer, one verdict) */
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 43, false), true, false, last, 3, 52, &who));
    /* Our own node-ID: not another allocator (and the controller never gives us our own frames anyway) */
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 52, false), true, false, first, 8, 52, &who));
    /* A request (anonymous), another subject, a service, an 11-bit or remote frame: no */
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 0x35, true), true, false, first, 8, 52, &who));
    CHECK(!cyphal_alloc_other_allocator(message_id(7509, 43, false), true, false, first, 8, 52, &who));
    CHECK(!cyphal_alloc_other_allocator((4u << 26) | (1u << 25) | (8166u << 14) | 43, true, false, first, 8, 52, &who));
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 43, false) & 0x7FF, false, false, first, 8, 52, &who));
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 43, false), true, true, first, 8, 52, &who));
    CHECK(!cyphal_alloc_other_allocator(message_id(8166, 43, false), true, false, first, 0, 52, &who));
    /* A single-frame answer (as a CAN FD allocator might send) counts too */
    const uint8_t single[2] = { 0, 0xE0 };
    CHECK(cyphal_alloc_other_allocator(message_id(8166, 7, false), true, false, single, 2, 52, &who) && who == 7);
}

static void test_blob(void)
{
    cyphal_alloc_row_t rows[CAP], back_rows[CAP], small_rows[2];
    cyphal_alloc_table_t t, back, small;
    cyphal_alloc_table_init(&t, rows, CAP);
    cyphal_alloc_table_init(&back, back_rows, CAP);
    cyphal_alloc_table_init(&small, small_rows, 2);
    uint8_t blob[CYPHAL_ALLOC_BLOB_SIZE(CAP)];
    uint8_t id;
    size_t dropped = 0;

    /* Empty: the version and a count of 0 */
    CHECK(cyphal_alloc_encode(&t, blob, sizeof(blob)) == 2 && blob[0] == CYPHAL_ALLOC_BLOB_VERSION && blob[1] == 0);
    CHECK(cyphal_alloc_decode(&back, blob, 2, &dropped) == CYPHAL_ALLOC_BLOB_OK && back.count == 0);

    cyphal_alloc_serve(&t, 0x0b3af4648a21ull, 100, 119, 52, NULL, NULL, &id);
    cyphal_alloc_serve(&t, 0xffffffffffffull, 100, 119, 52, NULL, NULL, &id);
    cyphal_alloc_heard(&t, 110, 100, 119, 52);
    const size_t n = cyphal_alloc_encode(&t, blob, sizeof(blob));
    CHECK(n == CYPHAL_ALLOC_BLOB_SIZE(3));
    CHECK(blob[2] == 0x21 && blob[7] == 0x0b && blob[8] == 100 && blob[9] == 0);
    CHECK(cyphal_alloc_encode(&t, blob, n - 1) == 0);
    /* Back, row for row */
    CHECK(cyphal_alloc_decode(&back, blob, n, &dropped) == CYPHAL_ALLOC_BLOB_OK && back.count == 3);
    CHECK(memcmp(back_rows, rows, 3 * sizeof(rows[0])) == 0);
    /* Into a smaller table: the first rows kept */
    CHECK(cyphal_alloc_decode(&small, blob, n, &dropped) == CYPHAL_ALLOC_BLOB_TRUNCATED && small.count == 2 && dropped == 1 &&
          small_rows[1].node_id == 101);

    /* Refused whole: another version, a size that disagrees with the count, a node-ID over 127, a node-ID or a
     * pseudo-ID twice, an unknown flag */
    uint8_t bad[sizeof(blob)];
    memcpy(bad, blob, n);
    bad[0] = 2;     /* a later version: not read, and the caller says it will be saved over */
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_NEWER && back.count == 0);
    bad[0] = 0;
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_BAD && back.count == 0);
    CHECK(cyphal_alloc_decode(&back, blob, n - 1, &dropped) == CYPHAL_ALLOC_BLOB_BAD);
    CHECK(cyphal_alloc_decode(&back, blob, 1, &dropped) == CYPHAL_ALLOC_BLOB_BAD);
    memcpy(bad, blob, n);
    bad[8] = 128;
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_BAD);
    memcpy(bad, blob, n);
    bad[16] = 100;                      /* row 1 given 100 too */
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_BAD);
    memcpy(bad, blob, n);
    memcpy(bad + 10, bad + 2, 6);       /* row 1 with row 0's pseudo-ID */
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_BAD && back.count == 0);
    memcpy(bad, blob, n);
    bad[9] = 0x02;
    CHECK(cyphal_alloc_decode(&back, bad, n, &dropped) == CYPHAL_ALLOC_BLOB_BAD);
    /* Two heard rows share pseudo-ID 0: that is no duplicate */
    cyphal_alloc_heard(&t, 111, 100, 119, 52);
    CHECK(cyphal_alloc_decode(&back, blob, cyphal_alloc_encode(&t, blob, sizeof(blob)), &dropped) == CYPHAL_ALLOC_BLOB_OK &&
          back.count == 4);
}

/* A saved table bigger than this build's: given rows kept before heard ones, whatever their order in the blob */
static void test_blob_given_first(void)
{
    cyphal_alloc_row_t rows[CAP], small_rows[2];
    cyphal_alloc_table_t t, small;
    cyphal_alloc_table_init(&t, rows, CAP);
    cyphal_alloc_table_init(&small, small_rows, 2);
    uint8_t blob[CYPHAL_ALLOC_BLOB_SIZE(CAP)], id;
    size_t dropped = 0;
    cyphal_alloc_heard(&t, 110, 100, 119, 52);
    cyphal_alloc_heard(&t, 111, 100, 119, 52);
    cyphal_alloc_serve(&t, 0xA, 100, 119, 52, NULL, NULL, &id);
    cyphal_alloc_serve(&t, 0xB, 100, 119, 52, NULL, NULL, &id);
    const size_t n = cyphal_alloc_encode(&t, blob, sizeof(blob));
    CHECK(cyphal_alloc_decode(&small, blob, n, &dropped) == CYPHAL_ALLOC_BLOB_TRUNCATED && dropped == 2);
    CHECK(small.count == 2 && !(small_rows[0].flags & CYPHAL_ALLOC_ROW_HEARD) && small_rows[0].node_id == 100 &&
          small_rows[1].node_id == 101);
}

/* A row whose node-ID is the allocator's own (it was given, then the allocator was set to it): refused */
static void test_own(void)
{
    cyphal_alloc_row_t rows[CAP];
    cyphal_alloc_table_t t;
    cyphal_alloc_table_init(&t, rows, CAP);
    uint8_t id = 0;
    CHECK(cyphal_alloc_serve(&t, 0xAAA, 100, 119, 52, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 100);
    CHECK(cyphal_alloc_serve(&t, 0xAAA, 100, 119, 100, NULL, NULL, &id) == CYPHAL_ALLOC_OWN && id == 100);
    /* and never picked for another */
    CHECK(cyphal_alloc_serve(&t, 0xBBB, 100, 119, 101, NULL, NULL, &id) == CYPHAL_ALLOC_NEW && id == 102);
}

typedef struct {
    const cyphal_alloc_recent_t *r;
    int64_t now;
} recent_ctx_t;

static bool busy_recent(void *ctx, uint8_t node_id)
{
    const recent_ctx_t *c = ctx;
    return cyphal_alloc_recent_has(c->r, node_id, c->now);
}

static void test_recent(void)
{
    cyphal_alloc_recent_t r;
    cyphal_alloc_recent_clear(&r);
    const int64_t S = 1000000;
    CHECK(!cyphal_alloc_recent_has(&r, 100, 0));
    cyphal_alloc_recent_note(&r, 100, 10 * S);
    CHECK(cyphal_alloc_recent_has(&r, 100, 10 * S) && cyphal_alloc_recent_has(&r, 100, 12 * S - 1));
    CHECK(!cyphal_alloc_recent_has(&r, 100, 12 * S) && !cyphal_alloc_recent_has(&r, 101, 10 * S));
    /* Noted again: its own entry moves on, the others stay */
    cyphal_alloc_recent_note(&r, 101, 11 * S);
    cyphal_alloc_recent_note(&r, 100, 11 * S);
    CHECK(cyphal_alloc_recent_has(&r, 100, 12 * S) && cyphal_alloc_recent_has(&r, 101, 12 * S));
    /* Full: the oldest goes */
    cyphal_alloc_recent_note(&r, 102, 11 * S + 1);
    cyphal_alloc_recent_note(&r, 103, 11 * S + 2);
    cyphal_alloc_recent_note(&r, 104, 11 * S + 3);
    CHECK(cyphal_alloc_recent_has(&r, 104, 12 * S) && cyphal_alloc_recent_has(&r, 102, 12 * S));
    CHECK(!cyphal_alloc_recent_has(&r, 100, 12 * S) && cyphal_alloc_recent_has(&r, 101, 12 * S));  /* the oldest went */
    /* As cyphal_alloc_pick()'s busy, the node does: a row given and forgotten at once is not given again while
     * recent, and is once the time has passed */
    cyphal_alloc_row_t rows[CAP];
    cyphal_alloc_table_t t;
    cyphal_alloc_table_init(&t, rows, CAP);
    cyphal_alloc_recent_clear(&r);
    recent_ctx_t ctx = { &r, 20 * S };
    uint8_t id = 0;
    CHECK(cyphal_alloc_serve(&t, 0xA, 100, 101, 52, busy_recent, &ctx, &id) == CYPHAL_ALLOC_NEW && id == 100);
    cyphal_alloc_recent_note(&r, id, ctx.now);
    cyphal_alloc_table_forget(&t, 100);
    CHECK(cyphal_alloc_serve(&t, 0xB, 100, 101, 52, busy_recent, &ctx, &id) == CYPHAL_ALLOC_NEW && id == 101);
    cyphal_alloc_table_clear(&t);
    ctx.now += CYPHAL_ALLOC_RECENT_US;
    CHECK(cyphal_alloc_serve(&t, 0xC, 100, 101, 52, busy_recent, &ctx, &id) == CYPHAL_ALLOC_NEW && id == 100);
}

int main(void)
{
    test_table();
    test_heard();
    test_reply();
    test_request();
    test_other_allocator();
    test_blob();
    test_blob_given_first();
    test_own();
    test_recent();
    printf("cyphal_alloc: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
