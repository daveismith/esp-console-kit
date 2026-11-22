/*
 * The parts of the Cyphal node with no hardware in them: plain C, no ESP-IDF, so that
 * tests/host builds and tests them on the computer (`make unit-test`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYPHAL_UNIQUE_ID_SIZE   16
#define CYPHAL_UID_PREFIX_SIZE  8      /* the constant part of the unique ID: names the firmware */
#define CYPHAL_NAME_MAX         50     /* uavcan.node.GetInfo.1.0's name: uint8[<=50] */
#define CYPHAL_PSEUDO_ID_MASK   0xFFFFFFFFFFFFull   /* uavcan.pnp.NodeIDAllocationData.1.0: truncated uint48 */
/* A read that finds the receive queue empty proves that every frame stamped this long before it
 * was read: the ISR stamps a frame a moment before it queues it, maybe on the other core */
#define CYPHAL_RX_SLACK_US      1000

#define CYPHAL_PORTS_MAX        16     /* the application's own ports a node keeps the IDs of (cyphal_registers.h) */

#define CYPHAL_NODE_ID_MAX      127
#define CYPHAL_NODE_ID_AUTO     255    /* the node-ID setting for plug-and-play */

typedef struct {
    uint8_t node_id;                    /* 0-127, or CYPHAL_NODE_ID_AUTO */
    char name[CYPHAL_NAME_MAX + 1];     /* bytes after the terminator are not part of it */
    uint32_t bitrate;                   /* 125000, 250000, 500000 or 1000000 */
    /* Each of the application's ports' subject- or service-ID, CYPHAL_PORT_OFF (65535) for off, in
     * its table's order (cyphal_reg_init()); 65535 in the slots past its last */
    uint16_t port_id[CYPHAL_PORTS_MAX];
} cyphal_settings_t;

/* The same settings: the name compared as a string, whatever follows its terminator */
bool cyphal_settings_equal(const cyphal_settings_t *a, const cyphal_settings_t *b);

/* A node-ID setting: 0 to CYPHAL_NODE_ID_MAX, or CYPHAL_NODE_ID_AUTO */
bool cyphal_node_id_valid(uint32_t node_id);

/* One of the bitrates the node runs at: 125000, 250000, 500000 or 1000000 */
bool cyphal_bitrate_valid(uint32_t bitrate);

/* The listen from the node's start before it sends as a node-ID (D9): a fixed node-ID's before going
 * online, plug-and-play's before taking up an allocated one. In esp_timer microseconds. It is over once
 * every frame stamped before its end has been checked, not merely once the time has passed: a
 * node task held off past the end (a flash write stalls both cores) may still have frames from
 * before it in the receive queue, and one of them may carry its own node-ID. The queue is in
 * stamp order, so a frame checked whose stamp is at or after the end proves it, as does a read
 * that finds the queue empty CYPHAL_RX_SLACK_US after the end; a busy bus can't hold it back. */
typedef struct {
    int64_t until_us;       /* the end */
    int64_t checked_us;     /* every frame stamped before this has been checked */
} cyphal_listen_t;

void cyphal_listen_start(cyphal_listen_t *l, int64_t now_us, int64_t length_us);
/* A frame stamped stamp_us (canbus_frame_t.timestamp_us) has been checked */
void cyphal_listen_frame(cyphal_listen_t *l, int64_t stamp_us);
/* A read begun at read_us found the receive queue empty */
void cyphal_listen_empty(cyphal_listen_t *l, int64_t read_us);
bool cyphal_listen_over(const cyphal_listen_t *l);
/* When an empty read would end it: the time to look again on a quiet bus */
int64_t cyphal_listen_wake_us(const cyphal_listen_t *l);

/* CRC-64/WE: poly 0x42F0E1EBA9EA3693, init and xorout all ones, not reflected. Its check value,
 * over "123456789", is 0x62EC59E3F1A4F00A. */
uint64_t cyphal_crc64we(const void *data, size_t len);

/* The pseudo-ID plug-and-play sends for a unique ID: the low 48 bits of its CRC-64/WE */
uint64_t cyphal_pseudo_id(const uint8_t unique_id[CYPHAL_UNIQUE_ID_SIZE]);

/* The unique ID (the kit's README, "cyphal"): bytes 0-7 the prefix, 8-9 zero, 10-15
 * the chip's base MAC address. Changing this changes every board's node-ID. */
void cyphal_unique_id(uint8_t out[CYPHAL_UNIQUE_ID_SIZE], const char prefix[CYPHAL_UID_PREFIX_SIZE],
                      const uint8_t mac[6]);

/* Whether `name` is a node name: 1 to 50 characters of a-z, 0-9, `_` and `.`, with no empty part
 * between dots and no part starting with a digit. False with the reason in `why`. */
bool cyphal_name_valid(const char *name, char *why, size_t why_len);

/* The source node-ID of a Cyphal/CAN v1.0 frame, or -1 when it has none to compare: an anonymous
 * message (its low bits are a pseudo-ID), or not a Cyphal frame at all (11-bit, remote, empty,
 * or a reserved bit set). */
int cyphal_frame_source(uint32_t can_id, bool ext, bool rtr, uint8_t len);

/* The classic CAN frames a transfer of `size` bytes takes: one up to 7 bytes; beyond, the bytes and a 2-byte
 * CRC, 7 a frame (each frame's eighth byte is the tail byte) */
size_t cyphal_frames(size_t size);

/* ------------------------------------------------------------------ plug-and-play */

/* uavcan.pnp.NodeIDAllocationData.1.0 (subject 8166), the allocatee's side of its rules A to D: requests
 * are anonymous single frames carrying the pseudo-ID, at random intervals (Trequest); the allocator
 * answers with a regular message carrying the pseudo-ID and the node-ID. */
#define CYPHAL_PNP_REQUEST_MIN_US     150000    /* Trequest: 150 ms, ... */
#define CYPHAL_PNP_REQUEST_JITTER_US  850000    /* ... plus up to 850 ms at random: within the 0 to 1 s the spec gives */

/* Trequest from a random number (esp_random() on the board): 150 ms to just under 1 s */
int64_t cyphal_pnp_interval_us(uint32_t random);

/* The request for a pseudo-ID (cyphal_pseudo_id()), serialized into `out`, which holds `size` bytes: at
 * least uavcan_pnp_NodeIDAllocationData_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_. Its size (7: the pseudo-ID
 * and an empty allocated_node_id, one anonymous frame), or 0 if `out` is smaller than that. */
size_t cyphal_pnp_request(uint8_t *out, size_t size, uint64_t pseudo_id);

typedef enum {
    CYPHAL_PNP_INVALID,     /* not a NodeIDAllocationData.1.0 that can be read (an array longer than 1): ignored */
    CYPHAL_PNP_OTHER,       /* an allocation message, but no node-ID for this node: rule C, the request timer restarts */
    CYPHAL_PNP_ALLOCATED,   /* this node's node-ID: rule D */
} cyphal_pnp_result_t;

/* What a NodeIDAllocationData.1.0 transfer (its payload; `anonymous` if its source was) means to the node
 * whose pseudo-ID is `pseudo_id`. It is this node's only from a regular source, for this pseudo-ID, with a
 * node-ID populated, its bytes all there, and one a CAN node can have (0-127): then *node_id is it. Another
 * node's request, another node's allocation, a reply with no node-ID, one cut short inside it (which the
 * deserializer would read as zeros: node-ID 0) or one out of range are CYPHAL_PNP_OTHER. */
cyphal_pnp_result_t cyphal_pnp_accept(const void *payload, size_t size, bool anonymous, uint64_t pseudo_id,
                                      uint8_t *node_id);

/* When each node-ID was last heard on the bus (a frame with it as the source), so that an allocated
 * node-ID another node is visibly using is not taken up. In esp_timer microseconds, as the frames' stamps. */
#define CYPHAL_NODE_IDS 128
typedef struct {
    int64_t last_us[CYPHAL_NODE_IDS];   /* INT64_MIN: never */
} cyphal_seen_t;

void cyphal_seen_clear(cyphal_seen_t *s);
/* A frame from `source` (cyphal_frame_source(): -1, for none, is ignored) stamped stamp_us */
void cyphal_seen_frame(cyphal_seen_t *s, int source, int64_t stamp_us);
/* Whether node_id has been heard at or after since_us */
bool cyphal_seen_since(const cyphal_seen_t *s, uint8_t node_id, int64_t since_us);
/* Whether node_id, allocated to this node, is another node's as of at_us: heard within a heartbeat's offline
 * timeout (3 s) before then, or at any time since. */
bool cyphal_pnp_in_use(const cyphal_seen_t *s, uint8_t node_id, int64_t at_us);

/* How long a node with plug-and-play listens from its start before it takes up an allocated node-ID: a
 * heartbeat's longest period (uavcan.node.Heartbeat.1.0's MAX_PUBLICATION_PERIOD, 1 s), so that a node
 * already on the bus with that node-ID has been heard at least once. An allocator answering the first
 * request does so 0.15 to 1 s after the start (Trequest), which alone would be too short. */
#define CYPHAL_PNP_LISTEN_US  1000000

typedef enum {
    CYPHAL_PNP_HOLD,    /* listened for less than CYPHAL_PNP_LISTEN_US since the start: keep the node-ID, keep listening */
    CYPHAL_PNP_TAKE,    /* nobody heard with it: go online on it */
    CYPHAL_PNP_TAKEN,   /* another node's (cyphal_pnp_in_use()): stop (IN_USE) without a frame sent as it */
} cyphal_pnp_verdict_t;

/* What to do with node_id, allocated to this node by a message stamped alloc_us. `l` is the listen begun at
 * the node's start for CYPHAL_PNP_LISTEN_US (cyphal_listen_start()) and fed every frame checked since, and
 * `s` the sources of those frames. Held until the listen is over; then in use if heard within the offline
 * timeout before the later of the allocation and the listen's end, or after. */
cyphal_pnp_verdict_t cyphal_pnp_verdict(const cyphal_seen_t *s, const cyphal_listen_t *l, uint8_t node_id,
                                        int64_t alloc_us);

/* ------------------------------------------------------------------ bus-off */

/* The node brings the controller back from bus-off itself, and backs off when the bus won't have it. The
 * first bus-off of a run is recovered from at once (at the node's next look at the bus, within a second).
 * One that comes within CYPHAL_BUSOFF_STABLE_US of the recovery before it is back to back: its recovery
 * waits CYPHAL_BUSOFF_FIRST_WAIT_US, and each after that twice the one before, up to
 * CYPHAL_BUSOFF_MAX_WAIT_US. A bus the node can't stay on at all (no working transceiver: it can't read
 * its own bits back) then costs a moment's errors every 30 s, not a bus-off every second. Logged at the
 * first bus-off of a run and then at the 2nd, 4th, 8th, ...: ever further apart. */
#define CYPHAL_BUSOFF_FIRST_WAIT_US  1000000
#define CYPHAL_BUSOFF_MAX_WAIT_US    30000000
#define CYPHAL_BUSOFF_STABLE_US      60000000

typedef struct {
    int64_t recovered_us;   /* when the last recovery was asked for; INT64_MIN for none yet */
    int64_t wait_us;        /* the wait before the last one, 0 when it was at once */
    uint32_t run;           /* bus-offs back to back, the latest included; 0 before the first */
} cyphal_busoff_t;

void cyphal_busoff_init(cyphal_busoff_t *b);
/* A new bus-off, seen at now_us: when to ask for its recovery (now_us, or later), and in *log whether to
 * log it (the first of a run, then each power of two) */
int64_t cyphal_busoff_seen(cyphal_busoff_t *b, int64_t now_us, bool *log);
/* Its recovery asked for at now_us */
void cyphal_busoff_recovering(cyphal_busoff_t *b, int64_t now_us);

/* uavcan.node.GetInfo's software version and VCS revision from the firmware's version, which is
 * `git describe --always --tags --dirty`: a release (vMAJOR.MINOR.PATCH, or a -rcN pre-release)
 * gives its major and minor, anything else 0.0; a commit hash in it (`-g<hash>`, or a bare hash)
 * gives the VCS revision, otherwise 0. */
void cyphal_software_version(const char *version, uint8_t *major, uint8_t *minor, uint64_t *vcs_revision);

#ifdef __cplusplus
}
#endif
