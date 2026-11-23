/*
 * The node as a plug-and-play allocator (CONFIG_CYPHAL_PNP_ALLOCATOR): it gives node-IDs to nodes that ask
 * by uavcan.pnp.NodeIDAllocationData.1.0 (subject 8166), from a range of its own, and remembers whom it gave
 * which (the kit's README, "cyphal"). Only built with the option on; with it off nothing here exists.
 *
 * The first half is plain C, no ESP-IDF, so that tests/host builds it (cyphal_alloc.c): the table, the reply,
 * the request's and the frames' reading, and the table's saved form. The second half is the node's
 * (cyphal.c): the setting, the table as the console edits it, the status.
 *
 * The protocol, from the type's own text (public_regulated_data_types, uavcan/pnp/8165 and 8166): an
 * allocatee asks with an anonymous single frame carrying a 48-bit hash of its unique ID (its pseudo-ID) and
 * no node-ID; the allocator answers each request at once with the same message, from its own node-ID, the
 * same pseudo-ID and the node-ID it gives. A request it can't serve (no room) gets no answer. The table only
 * grows: a known pseudo-ID always gets its row's node-ID back. A node heard on the bus with a node-ID in the
 * allocator's range and no row gets a row of its own with no pseudo-ID (the spec's "mock entry", here a row
 * marked heard), so that its node-ID is never given to another. The answer is 9 bytes: two frames on
 * classic CAN, which a regular (not anonymous) node may send.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cyphal_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CYPHAL_ALLOC_SUBJECT     8166   /* uavcan.pnp.NodeIDAllocationData.1.0's fixed subject-ID */
#define CYPHAL_ALLOC_REQUEST_SIZE 7     /* the request: the 48-bit pseudo-ID and an empty node-ID array */
#define CYPHAL_ALLOC_REPLY_SIZE   9     /* the answer: the pseudo-ID, an array of one, the node-ID's 16 bits */

/* A row: a pseudo-ID and the node-ID given for it; or, CYPHAL_ALLOC_ROW_HEARD, a node heard on the bus with a
 * node-ID in range that nobody was given here (pseudo_id 0, not a real one), kept so its ID is never given */
#define CYPHAL_ALLOC_ROW_HEARD  0x01
typedef struct {
    uint64_t pseudo_id;
    uint8_t node_id;
    uint8_t flags;
} cyphal_alloc_row_t;

/* The table: `count` rows in rows[0 .. capacity), in the order added; the caller owns the memory and locks it */
typedef struct {
    cyphal_alloc_row_t *rows;
    size_t count, capacity;
} cyphal_alloc_table_t;

void cyphal_alloc_table_init(cyphal_alloc_table_t *t, cyphal_alloc_row_t *rows, size_t capacity);
void cyphal_alloc_table_clear(cyphal_alloc_table_t *t);
/* The row given to `pseudo_id` (a heard row never matches): its index, -1 for none */
int cyphal_alloc_table_find(const cyphal_alloc_table_t *t, uint64_t pseudo_id);
/* The row with node_id, either kind: its index, -1 for none */
int cyphal_alloc_table_find_node(const cyphal_alloc_table_t *t, uint8_t node_id);
/* The row with node_id taken out (the rest keep their order): false if there was none */
bool cyphal_alloc_table_forget(cyphal_alloc_table_t *t, uint8_t node_id);

/* Whether a node-ID is in use on the bus as of now (the node's cyphal_pnp_in_use(): heard within a heartbeat's
 * offline timeout): never given while it is */
typedef bool (*cyphal_alloc_busy_fn)(void *ctx, uint8_t node_id);

/* The lowest node-ID in first..last (each 0-127) that no row has, isn't `own` and isn't busy (busy may be
 * NULL): -1 for none */
int cyphal_alloc_pick(const cyphal_alloc_table_t *t, uint8_t first, uint8_t last, uint8_t own,
                      cyphal_alloc_busy_fn busy, void *ctx);

typedef enum {
    CYPHAL_ALLOC_KNOWN,     /* the pseudo-ID has a row: its node-ID again */
    CYPHAL_ALLOC_NEW,       /* a row added for it, with the lowest free node-ID */
    CYPHAL_ALLOC_FULL,      /* no row free: no answer */
    CYPHAL_ALLOC_NO_ID,     /* a row free, but every node-ID in range is taken: no answer */
    CYPHAL_ALLOC_OWN,       /* the pseudo-ID's row has the allocator's own node-ID (it was set to that one after
                             * giving it, or a backup brought the row): no answer; *node_id is it */
} cyphal_alloc_result_t;

/* A request from `pseudo_id` served: its row's node-ID, or a new row (cyphal_alloc_pick()); *node_id the
 * node-ID to answer with when KNOWN or NEW */
cyphal_alloc_result_t cyphal_alloc_serve(cyphal_alloc_table_t *t, uint64_t pseudo_id, uint8_t first, uint8_t last,
                                         uint8_t own, cyphal_alloc_busy_fn busy, void *ctx, uint8_t *node_id);

/* A node heard on the bus with node_id: a heard row added if node_id is in first..last, isn't `own`, no row
 * has it and there is room. True if one was */
bool cyphal_alloc_heard(cyphal_alloc_table_t *t, uint8_t node_id, uint8_t first, uint8_t last, uint8_t own);

/* The answer to `pseudo_id`, giving node_id, serialized into `out` (`size` bytes): its size
 * (CYPHAL_ALLOC_REPLY_SIZE), or 0 if out is too small or node_id isn't 0-127 */
size_t cyphal_alloc_reply(uint8_t *out, size_t size, uint64_t pseudo_id, uint8_t node_id);

/* Whether a NodeIDAllocationData.1.0 transfer (its payload; `anonymous` if its source was) is a request an
 * allocator serves: anonymous, its 6 pseudo-ID bytes all there, and no node-ID. *pseudo_id then is it */
bool cyphal_alloc_request(const void *payload, size_t size, bool anonymous, uint64_t *pseudo_id);

/* Whether a received frame is the start of another allocator's answer: a Cyphal/CAN message frame on subject
 * 8166 from a regular source that isn't `own`, the first frame of its transfer. *source then is its node-ID.
 * Read from the frame alone, so a node can see it at any time, even before it reassembles transfers */
bool cyphal_alloc_other_allocator(uint32_t can_id, bool ext, bool rtr, const uint8_t *data, uint8_t len, uint8_t own,
                                  uint8_t *source);

/* The table's saved form (one NVS blob): a version byte (CYPHAL_ALLOC_BLOB_VERSION), the row count, then each
 * row as the pseudo-ID's 6 bytes (little-endian), the node-ID and the flags */
#define CYPHAL_ALLOC_BLOB_VERSION   1
#define CYPHAL_ALLOC_BLOB_ROW       8
#define CYPHAL_ALLOC_BLOB_SIZE(rows) (2 + CYPHAL_ALLOC_BLOB_ROW * (size_t)(rows))

/* The table into out (`size` bytes): its size, 0 if out is too small */
size_t cyphal_alloc_encode(const cyphal_alloc_table_t *t, uint8_t *out, size_t size);

typedef enum {
    CYPHAL_ALLOC_BLOB_OK,
    CYPHAL_ALLOC_BLOB_TRUNCATED,    /* more rows than the table holds: given rows kept first, then heard ones, in
                                     * their order, as many as fit; the rest counted in *dropped */
    CYPHAL_ALLOC_BLOB_NEWER,        /* a later version of the saved form: the table left empty (the node then
                                     * adds, answers and saves nothing until `cyphal allocations clear`) */
    CYPHAL_ALLOC_BLOB_BAD,          /* not a table of this version (size, version, a node-ID over 127, a
                                     * node-ID or a pseudo-ID twice): the table left empty */
} cyphal_alloc_blob_t;

/* The saved form read into t, replacing what it held */
cyphal_alloc_blob_t cyphal_alloc_decode(cyphal_alloc_table_t *t, const uint8_t *data, size_t size, size_t *dropped);

/* The node-IDs answered lately: an allocatee holds an allocation about 1 s before it sends anything as that node-ID,
 * so a row forgotten or cleared meanwhile would let the node-ID be given again with nobody heard using it. An
 * answered node-ID counts as taken for CYPHAL_ALLOC_RECENT_US. An entry younger than that is never replaced by
 * another node-ID: with every entry young (cyphal_alloc_recent_full()) the caller gives no new node-ID, rather than
 * forget one. The caller owns the entries; the node keeps twice its table's rows, which only forgets inside 2 s
 * could fill */
#define CYPHAL_ALLOC_RECENT_US  2000000
typedef struct {
    uint8_t node_id;
    int64_t at_us;                  /* INT64_MIN: none */
} cyphal_alloc_recent_entry_t;
typedef struct {
    cyphal_alloc_recent_entry_t *entries;
    size_t capacity;                /* 0 (no entries): always full, so nothing new is given */
} cyphal_alloc_recent_t;

void cyphal_alloc_recent_init(cyphal_alloc_recent_t *r, cyphal_alloc_recent_entry_t *entries, size_t capacity);
/* Every entry none */
void cyphal_alloc_recent_clear(cyphal_alloc_recent_t *r);
/* node_id answered at now_us: its own entry moved on, or a free or expired one taken. False, and nothing changed,
 * if it has none and every entry is younger than CYPHAL_ALLOC_RECENT_US */
bool cyphal_alloc_recent_note(cyphal_alloc_recent_t *r, uint8_t node_id, int64_t now_us);
/* Whether node_id was answered within CYPHAL_ALLOC_RECENT_US before now_us */
bool cyphal_alloc_recent_has(const cyphal_alloc_recent_t *r, uint8_t node_id, int64_t now_us);
/* Whether every entry is younger than CYPHAL_ALLOC_RECENT_US: a node-ID not yet in it couldn't be noted */
bool cyphal_alloc_recent_full(const cyphal_alloc_recent_t *r, int64_t now_us);

/* ------------------------------------------------------------------ the node's (cyphal.c) */

typedef enum {
    CYPHAL_ALLOCATOR_OFF,           /* the setting is off for this run, or the node isn't running (set at its stop) */
    CYPHAL_ALLOCATOR_NEEDS_FIXED,   /* on, but the node-ID is plug-and-play: an allocator needs a fixed one */
    CYPHAL_ALLOCATOR_LISTENING,     /* on: quiet through the fixed node-ID's listen */
    CYPHAL_ALLOCATOR_ACTIVE,        /* on: answering requests */
    CYPHAL_ALLOCATOR_YIELDED,       /* another allocator answered on the bus: quiet until the node's next start */
    CYPHAL_ALLOCATOR_STOPPED,       /* the node stopped (its node-ID in use): until the next start */
    CYPHAL_ALLOCATOR_RESET,         /* a reset (`cyphal reset`, a factory reset) turned it off: until the next start */
    CYPHAL_ALLOCATOR_NEWER_TABLE,   /* on, but the saved table is a later firmware's: it adds no row, answers nothing
                                     * and saves nothing, so as not to overwrite it, until `cyphal allocations clear`
                                     * and the node's next start */
} cyphal_allocator_state_t;

typedef struct {
    bool saved;                     /* the setting, from the next start */
    bool running;                   /* the setting as the node started; false while it doesn't run */
    cyphal_allocator_state_t state;
    uint8_t other;                  /* YIELDED: the other allocator's node-ID */
    uint8_t first, last;            /* the range (CONFIG_CYPHAL_ALLOCATOR_FIRST and _LAST) */
    size_t rows, heard_rows, capacity;  /* the table: rows, heard ones among them, of CONFIG_CYPHAL_ALLOCATOR_TABLE */
    /* Since the node started: requests heard (every one, the shed ones too), rows added for them, requests answered
     * from a row, requests that got no answer for want of room (a row, or a node-ID neither taken, in use nor
     * answered in the last 2 s), other allocators heard; requests whose row has the node's own node-ID
     * (unanswered); requests not answered because the transmit queue was nearly full (shed) */
    uint32_t requests, allocations, repeats, refused, yielded, own, shed;
    bool unsaved;                   /* a row added, or a save under way, not yet in NVS */
    bool newer_table;               /* the saved table is a later firmware's (CYPHAL_ALLOCATOR_NEWER_TABLE): kept
                                     * as it is until `cyphal allocations clear` */
} cyphal_allocator_status_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"

/* The setting, saved: in effect from the node's next start. Also the register CONFIG_CYPHAL_ALLOCATOR_REGISTER */
esp_err_t cyphal_set_allocator(bool on);
void cyphal_allocator_status(cyphal_allocator_status_t *out);
/* A copy of the table's rows, max at most, taken at one moment: how many */
size_t cyphal_allocator_rows(cyphal_alloc_row_t *out, size_t max);
/* A row taken out, saved: its node-ID free again (ESP_ERR_NOT_FOUND for none) */
esp_err_t cyphal_allocator_forget(uint8_t node_id);
/* Every row taken out, saved (over a later firmware's saved table too) */
esp_err_t cyphal_allocator_clear(void);
/* Whether register `index` (cyphal_reg()) is the allocator's: applied at the node's next start, though not
 * CYPHAL_REG_NEXT_START. Known by its write function, so an application register of the same name isn't it */
bool cyphal_allocator_is_register(size_t index);
#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
