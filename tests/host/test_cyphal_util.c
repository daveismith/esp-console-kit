/*
 * Host-built tests for cyphal/cyphal_util.c: the parts of the Cyphal node with no hardware in them.
 * `make -C tests/host` builds and runs them with the computer's cc; no ESP-IDF (the committed
 * Nunavut headers in cyphal/dsdl are plain C).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "cyphal_util.h"

static void test_crc64we(void)
{
    /* The catalogue's check value, and the empty message (init ^ xorout) */
    CHECK(cyphal_crc64we("123456789", 9) == 0x62EC59E3F1A4F00Aull);
    CHECK(cyphal_crc64we("", 0) == 0);
}

static void test_unique_id(void)
{
    /* holo-player-fw's example board (its manual/use/cyphal.md): MAC e4:b0:63:8c:2d:b0, holo-2db0 */
    static const uint8_t mac[6] = { 0xe4, 0xb0, 0x63, 0x8c, 0x2d, 0xb0 };
    static const uint8_t want[16] = { 0x68, 0x6f, 0x6c, 0x6f, 0x70, 0x6c, 0x61, 0x79,  /* holoplay */
                                      0x00, 0x00, 0xe4, 0xb0, 0x63, 0x8c, 0x2d, 0xb0 };
    uint8_t uid[16];
    memset(uid, 0xAA, sizeof(uid));
    cyphal_unique_id(uid, "holoplay", mac);
    CHECK(memcmp(uid, want, sizeof(want)) == 0);

    /* The pseudo-ID is the low 48 bits of the CRC over all 16 bytes */
    const uint64_t crc = cyphal_crc64we(uid, sizeof(uid));
    CHECK(cyphal_pseudo_id(uid) == (crc & 0xFFFFFFFFFFFFull));
    CHECK(cyphal_pseudo_id(uid) <= 0xFFFFFFFFFFFFull);
    /* One bit of the MAC changes it */
    uint8_t other[16];
    memcpy(other, uid, sizeof(other));
    other[15] ^= 1;
    CHECK(cyphal_pseudo_id(other) != cyphal_pseudo_id(uid));
    printf("  example board: unique id 686f6c6f706c61790000e4b0638c2db0, pseudo-ID 0x%012" PRIx64 "\n",
           cyphal_pseudo_id(uid));

    /* The bench board (MAC 3c:0f:02:6f:2d:b0), whose pseudo-ID an allocator's table holds */
    static const uint8_t bench_mac[6] = { 0x3c, 0x0f, 0x02, 0x6f, 0x2d, 0xb0 };
    cyphal_unique_id(uid, "holoplay", bench_mac);
    CHECK(cyphal_pseudo_id(uid) == 0x0b3af4648a21ull);
}

static void test_names(void)
{
    char why[128];
    static const char *const good[] = {
        "r2.dome.holo.front", "r2.dome.holo.top", "r2.dome.holo.rear", "a", "a_b.c_d", "_x", "x.y9", "abc.d_1",
        /* 50 characters */
        "abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdef",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        why[0] = '\0';
        const bool ok = cyphal_name_valid(good[i], why, sizeof(why));
        if (!ok) {
            printf("  `%s` refused: %s\n", good[i], why);
        }
        CHECK(ok);
    }
    struct {
        const char *name;
        const char *reason;     /* a part of the reason it must give */
    } bad[] = {
        { "", "1 to 50 characters, and this is 0" },
        { "abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefg", "and this is 51" },
        { "R2.Dome", "`R` is not allowed" },
        { "r2..holo", "empty" },
        { "r2.1holo", "start with a digit: `1holo`" },
        { "1r2", "start with a digit: `1r2`" },
        { ".r2", "empty" },
        { "r2.", "empty" },
        { "r2 dome", "character 0x20" },
        { "r2-dome", "`-` is not allowed" },
        { "r2.dome\xc3\xa9", "character 0xC3" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = '\0';
        const bool ok = cyphal_name_valid(bad[i].name, why, sizeof(why));
        CHECK(!ok);
        if (strstr(why, bad[i].reason) == NULL) {
            printf("  `%s`: reason \"%s\" lacks \"%s\"\n", bad[i].name, why, bad[i].reason);
            CHECK(0);
        }
    }
}

static void test_frame_source(void)
{
    /* Heartbeat from node 52: priority 4, subject 7509, bits 21-22 set as transmitted */
    const uint32_t hb = (4u << 26) | (3u << 21) | (7509u << 8) | 52u;
    CHECK(cyphal_frame_source(hb, true, false, 8) == 52);
    /* Anonymous (bit 24): the low bits are a pseudo-ID, not a node-ID */
    CHECK(cyphal_frame_source(hb | (1u << 24), true, false, 8) == -1);
    /* A service request from 10 to 52 (GetInfo, 430) and its response from 52 to 10 */
    const uint32_t req = (4u << 26) | (1u << 25) | (1u << 24) | (430u << 14) | (52u << 7) | 10u;
    const uint32_t resp = (4u << 26) | (1u << 25) | (430u << 14) | (10u << 7) | 52u;
    CHECK(cyphal_frame_source(req, true, false, 1) == 10);
    CHECK(cyphal_frame_source(resp, true, false, 8) == 52);
    /* Not Cyphal: 11-bit, remote, no tail byte, reserved bit 23, or bit 7 of a message */
    CHECK(cyphal_frame_source(0x123, false, false, 8) == -1);
    CHECK(cyphal_frame_source(hb, true, true, 0) == -1);
    CHECK(cyphal_frame_source(hb, true, false, 0) == -1);
    CHECK(cyphal_frame_source(hb | (1u << 23), true, false, 8) == -1);
    CHECK(cyphal_frame_source(req | (1u << 23), true, false, 8) == -1);
    CHECK(cyphal_frame_source(hb | (1u << 7), true, false, 8) == -1);
    /* Node 0 and node 127 are node-IDs like any other */
    CHECK(cyphal_frame_source((hb & ~0x7Fu) | 0, true, false, 8) == 0);
    CHECK(cyphal_frame_source((hb & ~0x7Fu) | 127, true, false, 8) == 127);
}

/* A transfer's frames on classic CAN: one up to 7 bytes, then the bytes and a 2-byte CRC, 7 a frame */
static void test_frames(void)
{
    CHECK(cyphal_frames(0) == 1 && cyphal_frames(1) == 1 && cyphal_frames(7) == 1);
    CHECK(cyphal_frames(8) == 2 && cyphal_frames(12) == 2 && cyphal_frames(13) == 3 && cyphal_frames(14) == 3);
    CHECK(cyphal_frames(86) == 13 && cyphal_frames(116) == 17 && cyphal_frames(265) == 39 && cyphal_frames(375) == 54);
}

static void test_software_version(void)
{
    struct {
        const char *version;
        uint8_t major, minor;
        uint64_t vcs;
    } cases[] = {
        { "v1.1.0", 1, 1, 0 },
        { "v0.2.0-rc1", 0, 2, 0 },
        { "v12.34.5", 12, 34, 0 },
        { "v1.1.0-1-gac95a11-dirty", 0, 0, 0xac95a11 },
        { "v1.1.0-1-gac95a11", 0, 0, 0xac95a11 },
        { "v1.1.0-dirty", 0, 0, 0 },
        { "v0.2.0-rc1-3-g0123abcd", 0, 0, 0x0123abcd },
        { "ac95a11", 0, 0, 0xac95a11 },
        { "ac95a11-dirty", 0, 0, 0xac95a11 },
        { "0123456789abcdef0123", 0, 0, 0x0123456789abcdefull },   /* 16 digits at most */
        { "v1.1", 0, 0, 0 },
        { "", 0, 0, 0 },
        { "1.0.0", 0, 0, 0 },
        { "v1.0.0-gold", 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t major = 99, minor = 99;
        uint64_t vcs = 99;
        cyphal_software_version(cases[i].version, &major, &minor, &vcs);
        const bool ok = major == cases[i].major && minor == cases[i].minor && vcs == cases[i].vcs;
        if (!ok) {
            printf("  `%s` gave %u.%u, vcs 0x%" PRIx64 "\n", cases[i].version, major, minor, vcs);
        }
        CHECK(ok);
    }
    uint8_t major = 9, minor = 9;
    uint64_t vcs = 9;
    cyphal_software_version(NULL, &major, &minor, &vcs);
    CHECK(major == 0 && minor == 0 && vcs == 0);
}

static void test_settings_equal(void)
{
    cyphal_settings_t a, b;
    memset(&a, 0, sizeof(a));
    a.node_id = 52;
    a.bitrate = 1000000;
    strcpy(a.name, "r2.dome.holo.front");
    /* What a longer name and then the old one leave behind the terminator (the console's
     * `cyphal name`, copying into the saved settings) is not part of the name */
    memcpy(&b, &a, sizeof(b));
    strcpy(b.name, "r2.dome.holo.front.longer");
    strcpy(b.name, "r2.dome.holo.front");
    CHECK(memcmp(&a, &b, sizeof(a)) != 0);
    CHECK(cyphal_settings_equal(&a, &b));
    CHECK(cyphal_settings_equal(&b, &a));
    /* Each field counts */
    b.node_id = 53;
    CHECK(!cyphal_settings_equal(&a, &b));
    b.node_id = 52;
    b.bitrate = 500000;
    CHECK(!cyphal_settings_equal(&a, &b));
    b.bitrate = 1000000;
    strcpy(b.name, "r2.dome.holo.fron");
    CHECK(!cyphal_settings_equal(&a, &b));
    strcpy(b.name, "r2.dome.holo.front.x");
    CHECK(!cyphal_settings_equal(&a, &b));
    /* A name of the full 50 characters, which fills the buffer up to its terminator */
    memset(a.name, 'a', CYPHAL_NAME_MAX);
    a.name[CYPHAL_NAME_MAX] = '\0';
    memcpy(b.name, a.name, sizeof(b.name));
    CHECK(cyphal_settings_equal(&a, &b));
    b.name[CYPHAL_NAME_MAX - 1] = 'b';
    CHECK(!cyphal_settings_equal(&a, &b));
}

/* D9's listen, in esp_timer microseconds: started at 10 s, 2 s long */
static void test_listen(void)
{
    const int64_t t0 = 10000000, len = 2000000, end = t0 + len;
    cyphal_listen_t l;

    /* A quiet bus: reads that find nothing end it only CYPHAL_RX_SLACK_US after its end, since
     * a frame stamped just before a read may not be in the queue yet */
    cyphal_listen_start(&l, t0, len);
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_empty(&l, t0 + 1000000);
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_empty(&l, end);
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_empty(&l, end + CYPHAL_RX_SLACK_US - 1);
    CHECK(!cyphal_listen_over(&l));
    CHECK(cyphal_listen_wake_us(&l) == end + CYPHAL_RX_SLACK_US);
    cyphal_listen_empty(&l, cyphal_listen_wake_us(&l));
    CHECK(cyphal_listen_over(&l));

    /* The task held off far past the end: time alone doesn't end it. The frames from before the
     * end are read first, and one stamped at or after it, or an empty read, ends it */
    cyphal_listen_start(&l, t0, len);
    cyphal_listen_frame(&l, end - 5000);
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_frame(&l, end - 1);
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_frame(&l, end);
    CHECK(cyphal_listen_over(&l));

    cyphal_listen_start(&l, t0, len);
    cyphal_listen_frame(&l, end - 1);
    cyphal_listen_empty(&l, end + 5000000);
    CHECK(cyphal_listen_over(&l));

    /* A busy bus doesn't hold it back: the first frame stamped after the end ends it, however
     * many follow */
    cyphal_listen_start(&l, t0, len);
    for (int64_t t = t0; t < end; t += 130) {
        cyphal_listen_frame(&l, t);
    }
    CHECK(!cyphal_listen_over(&l));
    cyphal_listen_frame(&l, end + 70);
    CHECK(cyphal_listen_over(&l));

    /* Nothing moves it back: an older stamp, or an empty read just after a late frame */
    cyphal_listen_start(&l, t0, len);
    cyphal_listen_frame(&l, end + 10);
    cyphal_listen_frame(&l, t0 + 1);
    cyphal_listen_empty(&l, end + 20);
    CHECK(cyphal_listen_over(&l));
    CHECK(l.checked_us == end + 10);

    /* A stamp from before the start (a frame queued as the controller started) is checked like any
     * other and ends nothing */
    cyphal_listen_start(&l, t0, len);
    cyphal_listen_frame(&l, t0 - 100);
    CHECK(!cyphal_listen_over(&l));
    CHECK(l.checked_us == t0);
}

/* This board's pseudo-ID (the bench board, MAC 3c:0f:02:6f:2d:b0) */
#define BENCH_PSEUDO_ID 0x0b3af4648a21ull

/* A NodeIDAllocationData.1.0 as an allocator writes it: the 48-bit hash little-endian, the array's
 * length, and each node-ID as a little-endian uint16 */
static size_t allocation(uint8_t out[16], uint64_t hash, int count, uint16_t id)
{
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(hash >> (8 * i));
    }
    out[6] = (uint8_t)count;
    out[7] = (uint8_t)id;
    out[8] = (uint8_t)(id >> 8);
    return count == 0 ? 7 : 9;
}

static void test_pnp_request(void)
{
    uint8_t req[16];
    memset(req, 0xAA, sizeof(req));
    /* 7 bytes and no more: libcanard sends an anonymous message only as a single frame */
    CHECK(cyphal_pnp_request(req, sizeof(req), BENCH_PSEUDO_ID) == 7);
    static const uint8_t want[7] = { 0x21, 0x8a, 0x64, 0xf4, 0x3a, 0x0b, 0x00 };
    CHECK(memcmp(req, want, sizeof(want)) == 0);
    CHECK(req[7] == 0xAA);
    /* Bits above 48 are not part of it */
    CHECK(cyphal_pnp_request(req, sizeof(req), BENCH_PSEUDO_ID | 0xFFFF000000000000ull) == 7);
    CHECK(memcmp(req, want, sizeof(want)) == 0);
    /* A buffer smaller than the type's: nothing */
    CHECK(cyphal_pnp_request(req, 8, BENCH_PSEUDO_ID) == 0);
    /* The request, read back as another allocatee would see it: anonymous, so not an allocation */
    uint8_t id = 99;
    CHECK(cyphal_pnp_accept(req, 7, true, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    CHECK(id == 99);
}

static void test_pnp_accept(void)
{
    uint8_t m[16];
    uint8_t id;
    size_t n;

    /* Ours: a regular message, our pseudo-ID, one node-ID */
    n = allocation(m, BENCH_PSEUDO_ID, 1, 2);
    id = 99;
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_ALLOCATED && id == 2);
    /* 0 and 127 are node-IDs like any other; the caller's bits above 48 are ignored */
    n = allocation(m, BENCH_PSEUDO_ID, 1, 0);
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_ALLOCATED && id == 0);
    n = allocation(m, BENCH_PSEUDO_ID, 1, 127);
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID | (1ull << 60), &id) == CYPHAL_PNP_ALLOCATED && id == 127);
    /* Bytes past the type's 9 (an extent from a newer minor version) are ignored */
    n = allocation(m, BENCH_PSEUDO_ID, 1, 45);
    m[9] = 0x55;
    CHECK(cyphal_pnp_accept(m, 10, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_ALLOCATED && id == 45);

    /* Rule C only (an allocation message, not ours): the timer restarts, nothing is taken */
    id = 99;
    n = allocation(m, BENCH_PSEUDO_ID ^ 1, 1, 2);       /* another node's allocation */
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    n = allocation(m, BENCH_PSEUDO_ID, 1, 2);           /* anonymous: an allocatee, whatever it carries */
    CHECK(cyphal_pnp_accept(m, n, true, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    n = allocation(m, 0x123456789abcull, 0, 0);         /* another allocatee's request */
    CHECK(cyphal_pnp_accept(m, n, true, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    n = allocation(m, BENCH_PSEUDO_ID, 0, 0);           /* a regular reply with no node-ID */
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    n = allocation(m, BENCH_PSEUDO_ID, 1, 128);         /* node-IDs a CAN node can't have */
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    n = allocation(m, BENCH_PSEUDO_ID, 1, 0xFFFF);
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    /* Cut short: what is missing reads as zeros (the array empty), as the spec's implicit extension says;
     * but an array of one whose node-ID's bytes are missing, or half there, is not an allocation of node 0 */
    id = 99;
    n = allocation(m, BENCH_PSEUDO_ID, 1, 2);
    CHECK(cyphal_pnp_accept(m, 6, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    CHECK(cyphal_pnp_accept(m, 7, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);    /* the node-ID's bytes gone */
    CHECK(cyphal_pnp_accept(m, 8, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);    /* one of its two */
    CHECK(cyphal_pnp_accept(m, 3, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    CHECK(cyphal_pnp_accept(m, 0, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_OTHER);
    CHECK(id == 99);
    n = allocation(m, BENCH_PSEUDO_ID, 1, 0);           /* node 0, whole: taken */
    m[7] = m[8] = 0;
    CHECK(cyphal_pnp_accept(m, 9, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_ALLOCATED && id == 0);

    /* Malformed: an array longer than its capacity of 1 can't be read; ignored, no timer restart */
    id = 99;
    n = allocation(m, BENCH_PSEUDO_ID, 2, 2);
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_INVALID);
    n = allocation(m, BENCH_PSEUDO_ID, 255, 2);
    CHECK(cyphal_pnp_accept(m, n, false, BENCH_PSEUDO_ID, &id) == CYPHAL_PNP_INVALID);
    CHECK(id == 99);
}

static void test_pnp_interval(void)
{
    /* 150 ms to just under 1 s, whatever the random number */
    CHECK(cyphal_pnp_interval_us(0) == CYPHAL_PNP_REQUEST_MIN_US);
    CHECK(cyphal_pnp_interval_us(CYPHAL_PNP_REQUEST_JITTER_US - 1) == 999999);
    CHECK(cyphal_pnp_interval_us(CYPHAL_PNP_REQUEST_JITTER_US) == CYPHAL_PNP_REQUEST_MIN_US);
    int64_t lo = INT64_MAX, hi = 0;
    uint32_t r = 0x12345678;
    for (int i = 0; i < 100000; i++) {
        r = r * 1664525u + 1013904223u;
        const int64_t t = cyphal_pnp_interval_us(r);
        lo = t < lo ? t : lo;
        hi = t > hi ? t : hi;
    }
    const int64_t top = cyphal_pnp_interval_us(UINT32_MAX);
    CHECK(lo >= 150000 && hi < 1000000 && top >= 150000 && top < 1000000);
    CHECK(lo < 160000 && hi > 990000);      /* and it uses the whole range */
}

static void test_seen(void)
{
    cyphal_seen_t s;
    cyphal_seen_clear(&s);
    /* Never heard: not even with a window reaching back before the board started */
    CHECK(!cyphal_seen_since(&s, 52, INT64_MIN + 1));
    CHECK(!cyphal_seen_since(&s, 52, -3000000));
    cyphal_seen_frame(&s, -1, 5000000);             /* no source: ignored */
    for (int i = 0; i < CYPHAL_NODE_IDS; i++) {
        CHECK(!cyphal_seen_since(&s, (uint8_t)i, -3000000));
    }
    cyphal_seen_frame(&s, 52, 5000000);
    CHECK(cyphal_seen_since(&s, 52, 5000000 - 3000000));
    CHECK(cyphal_seen_since(&s, 52, 5000000));
    CHECK(!cyphal_seen_since(&s, 52, 5000001));
    CHECK(!cyphal_seen_since(&s, 53, 0));
    cyphal_seen_frame(&s, 52, 4000000);             /* an older stamp doesn't move it back */
    CHECK(cyphal_seen_since(&s, 52, 4500000));
    cyphal_seen_frame(&s, 127, 1);
    cyphal_seen_frame(&s, 0, 1);
    CHECK(cyphal_seen_since(&s, 127, 0) && cyphal_seen_since(&s, 0, 1));
    CHECK(!cyphal_seen_since(&s, 128, INT64_MIN + 1));      /* not a node-ID */
    cyphal_seen_frame(&s, 128, 9000000);                    /* nor is this */
    cyphal_seen_clear(&s);
    CHECK(!cyphal_seen_since(&s, 52, 0));
}

/* An allocated node-ID heard in the 3 s before the allocation (a heartbeat's offline timeout) is another
 * node's: in use. Heard longer ago, or never, or only another node-ID heard: the node goes online. */
static void test_pnp_in_use(void)
{
    cyphal_seen_t s;
    cyphal_seen_clear(&s);
    CHECK(!cyphal_pnp_in_use(&s, 125, 0));                  /* never heard, the board just started */
    CHECK(!cyphal_pnp_in_use(&s, 125, 10000000));
    cyphal_seen_frame(&s, 125, 7000000);
    cyphal_seen_frame(&s, 124, 9999999);
    CHECK(cyphal_pnp_in_use(&s, 125, 7000000));             /* heard in the same instant */
    CHECK(cyphal_pnp_in_use(&s, 125, 9999999));
    CHECK(cyphal_pnp_in_use(&s, 125, 10000000));            /* 3 s exactly */
    CHECK(!cyphal_pnp_in_use(&s, 125, 10000001));           /* 3 s and a microsecond */
    CHECK(!cyphal_pnp_in_use(&s, 123, 10000000));           /* another node-ID heard */
    CHECK(cyphal_pnp_in_use(&s, 124, 12999999));
    /* Heard soon after the board started: the window reaches back before esp_timer's zero */
    cyphal_seen_clear(&s);
    cyphal_seen_frame(&s, 2, 100);
    CHECK(cyphal_pnp_in_use(&s, 2, 200000));
    CHECK(!cyphal_pnp_in_use(&s, 3, 200000));
}

/* The node fed a frame as cyphal.c's on_frame() does: the listen and the table both */
static void heard(cyphal_listen_t *l, cyphal_seen_t *s, int source, int64_t stamp)
{
    cyphal_listen_frame(l, stamp);
    cyphal_seen_frame(s, source, stamp);
}

/* An allocated node-ID is held until the node has listened a heartbeat period since its start (started at
 * 10 s here), then taken up unless another node was heard with it */
static void test_pnp_verdict(void)
{
    const int64_t T0 = 10000000, END = T0 + CYPHAL_PNP_LISTEN_US;
    CHECK(CYPHAL_PNP_LISTEN_US == 1000000);
    cyphal_listen_t l;
    cyphal_seen_t s;

    /* The allocation 0.3 s after the start, nobody else on the bus: held until the listen's end, by the
     * frames' stamps or an empty read after it, never by the allocation's own */
    cyphal_listen_start(&l, T0, CYPHAL_PNP_LISTEN_US);
    cyphal_seen_clear(&s);
    heard(&l, &s, 43, T0 + 300000);                         /* the allocator's reply */
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_HOLD);
    heard(&l, &s, 43, END - 1);                             /* the allocator's heartbeat */
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_HOLD);
    cyphal_listen_empty(&l, END + CYPHAL_RX_SLACK_US - 1);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_HOLD);
    cyphal_listen_empty(&l, END + CYPHAL_RX_SLACK_US);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_TAKE);
    /* or a frame stamped at the end */
    cyphal_listen_start(&l, T0, CYPHAL_PNP_LISTEN_US);
    heard(&l, &s, 43, END);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_TAKE);

    /* Another node with it, heard only after the allocation, during the hold: in use. Without the hold the
     * board would have gone online at 0.3 s and sent as it until that node's frame stopped it */
    cyphal_listen_start(&l, T0, CYPHAL_PNP_LISTEN_US);
    cyphal_seen_clear(&s);
    heard(&l, &s, 125, T0 + 900000);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_HOLD);
    heard(&l, &s, 43, END + 5);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_TAKEN);
    CHECK(cyphal_pnp_verdict(&s, &l, 124, T0 + 300000) == CYPHAL_PNP_TAKE);    /* another node-ID: free */
    /* heard just after the end, in a frame checked before the verdict: in use too */
    cyphal_listen_start(&l, T0, CYPHAL_PNP_LISTEN_US);
    cyphal_seen_clear(&s);
    heard(&l, &s, 125, END + 200);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 300000) == CYPHAL_PNP_TAKEN);

    /* An allocation after the listen is over (no allocator until then) is decided at once, against the 3 s
     * before it: a node heard 2.5 s earlier is still there, one heard 3.5 s earlier has gone */
    cyphal_listen_start(&l, T0, CYPHAL_PNP_LISTEN_US);
    cyphal_seen_clear(&s);
    heard(&l, &s, 125, T0 + 500000);
    heard(&l, &s, 43, T0 + 3000000);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 3000000) == CYPHAL_PNP_TAKEN);
    heard(&l, &s, 43, T0 + 4000000);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 3500000) == CYPHAL_PNP_TAKEN);   /* 3 s exactly */
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 3500001) == CYPHAL_PNP_TAKE);
    CHECK(cyphal_pnp_verdict(&s, &l, 125, T0 + 4000000) == CYPHAL_PNP_TAKE);
}

static void test_busoff(void)
{
    const int64_t S = 1000000;
    cyphal_busoff_t b;
    bool log = false;
    cyphal_busoff_init(&b);

    /* The first: at once, logged */
    int64_t t = 100 * S;
    CHECK(cyphal_busoff_seen(&b, t, &log) == t && log && b.run == 1);
    cyphal_busoff_recovering(&b, t);

    /* Back to back (the bus-off seen at the next look, a second later): 1, 2, 4, 8, 16, then 30 s at most */
    const int64_t waits[] = { 1, 2, 4, 8, 16, 30, 30, 30 };
    const bool logged[] = { true, false, true, false, false, false, true, false };    /* run 2, 3, 4, ... 9 */
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
        t += S;
        const int64_t at = cyphal_busoff_seen(&b, t, &log);
        CHECK(at == t + waits[i] * S);
        CHECK(log == logged[i]);
        CHECK(b.run == i + 2);
        t = at;
        cyphal_busoff_recovering(&b, t);
    }
    /* Logged ever further apart: the 16th, not the 15th */
    for (int run = 10; run <= 16; run++) {
        t += S;
        t = cyphal_busoff_seen(&b, t, &log);
        CHECK(log == (run == 16));
        cyphal_busoff_recovering(&b, t);
    }

    /* Up for just under a minute after the last recovery: still back to back, at the cap */
    CHECK(cyphal_busoff_seen(&b, t + CYPHAL_BUSOFF_STABLE_US - 1, &log) == t + CYPHAL_BUSOFF_STABLE_US - 1 + 30 * S);
    t += CYPHAL_BUSOFF_STABLE_US - 1 + 30 * S;
    cyphal_busoff_recovering(&b, t);
    /* A minute up: a new run, at once and logged; the next back to back starts again at 1 s */
    t += CYPHAL_BUSOFF_STABLE_US;
    CHECK(cyphal_busoff_seen(&b, t, &log) == t && log && b.run == 1);
    cyphal_busoff_recovering(&b, t);
    CHECK(cyphal_busoff_seen(&b, t + S, &log) == t + 2 * S && log && b.run == 2);

    /* A recovery asked for again (the bus-off outlasting it) counts from the last ask */
    cyphal_busoff_init(&b);
    CHECK(cyphal_busoff_seen(&b, 0, &log) == 0 && log);
    cyphal_busoff_recovering(&b, 0);
    cyphal_busoff_recovering(&b, 50 * S);
    CHECK(cyphal_busoff_seen(&b, 60 * S, &log) == 61 * S);    /* 10 s after the last ask: back to back */
}

int main(void)
{
    test_crc64we();
    test_unique_id();
    test_names();
    test_frame_source();
    test_frames();
    test_software_version();
    test_settings_equal();
    test_listen();
    test_pnp_request();
    test_pnp_accept();
    test_pnp_interval();
    test_seen();
    test_pnp_in_use();
    test_pnp_verdict();
    test_busoff();
    printf("cyphal_util: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
