/*
 * The node's registers: what uavcan.register.Access and uavcan.register.List serve, and what the
 * console's `cyphal register` reads and writes (the kit's README, "cyphal"). A table of
 * descriptors (cyphal_register_t), each with its name, type, flags and the functions that read and
 * write it. Plain C, no ESP-IDF: tests/host builds it.
 *
 * The node's own entries come first, over its saved settings (cyphal_settings_t), so that a register
 * and the `cyphal` command that sets the same thing are one value. In List's order:
 *   0  uavcan.node.id      natural16[1]  0-127, or 65535 for plug-and-play
 *   1  the name            string        cyphal_name_valid()'s rules; its register name is the
 *                                        application's (CONFIG_CYPHAL_NAME_REGISTER: r2.node.name on the droid's boards)
 *   2  uavcan.can.bitrate  natural32[2]  the first 125000, 250000, 500000 or 1000000; the second
 *                                        (CAN FD's data phase) ignored, and read back as the first
 * then, for each of the application's ports in its table's order, its `uavcan.<kind>.<name>.id` (kind pub,
 * sub, srv, or cln for a service it calls)
 * (natural16[1]: a subject-ID 0-8191 or a service-ID 0-511 that no other port has, by
 * cyphal_port_id_free()'s rule, or 65535 for off) and its
 * `uavcan.<kind>.<name>.type` (string, the port's type, immutable). All of these are persistent;
 * the mutable ones are saved as they are written and apply from the node's next start
 * (CYPHAL_REG_NEXT_START). Then the application's own registers (cyphal_reg_init()), each with the
 * flags and the effect its descriptor gives.
 *
 * A write is converted to the register's type where that is exact: any integer, natural or real
 * array whose elements are whole numbers, for a numeric register; a string or unstructured bytes, for
 * a string; a bit array, or numbers that are each 0 or 1, for a bit register. Anything else is
 * refused, and the register keeps its value.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cyphal_util.h"
#include "uavcan/_register/Value_1_0.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CYPHAL_PORT_OFF        65535   /* a port register's value for a port that is off */
#define CYPHAL_SUBJECT_ID_MAX  8191
#define CYPHAL_SERVICE_ID_MAX  511
#define CYPHAL_NVS_KEY_MAX     15      /* NVS's longest key */

/* Cyphal's transfer priorities, 0 the most urgent: a port table's tokens (holo-player-fw's r2/holo/ports.h, say) */
typedef enum {
    CYPHAL_PRIORITY_EXCEPTIONAL, CYPHAL_PRIORITY_IMMEDIATE, CYPHAL_PRIORITY_FAST, CYPHAL_PRIORITY_HIGH,
    CYPHAL_PRIORITY_NOMINAL, CYPHAL_PRIORITY_LOW, CYPHAL_PRIORITY_SLOW, CYPHAL_PRIORITY_OPTIONAL,
} cyphal_priority_t;

typedef enum {
    CYPHAL_PORT_PUB,    /* a subject the node publishes */
    CYPHAL_PORT_SUB,    /* a subject it subscribes to */
    CYPHAL_PORT_SRV,    /* a service it serves */
    /* A service it calls (cyphal_client.h): only with CONFIG_CYPHAL_CLIENT. Without it nothing handles one, and the
     * code is as it was before the kind existed (so that the library's objects don't change): its ID register checks
     * its ID as a subject's (0-8191, in the subjects' set), and its handler is checked as a publication's, so it is
     * refused and logged ("publish_max is 1 to CYPHAL_PUBLISH_MAX") unless it sets publish_max, and then kept, never
     * subscribed, and listed among port.List's subscribers. Put one in a table only with the option */
    CYPHAL_PORT_CLI,
} cyphal_port_kind_t;

/* One of the application's own ports: one whose ID is a register, with a default */
typedef struct {
    cyphal_port_kind_t kind;
    const char *name;               /* "status": the registers' PORT_NAME */
    const char *type;               /* "r2.holo.Status.0.1": the .type register's value */
    uint16_t default_id;
    cyphal_priority_t priority;     /* what the node sends at */
    const char *id_register;        /* "uavcan.pub.status.id" */
    const char *type_register;      /* "uavcan.pub.status.type" */
    const char *nvs_key;            /* "pub.status": where its ID is saved, CYPHAL_NVS_KEY_MAX at most */
} cyphal_port_t;

/* A cyphal_port_t from one row of a generated port table, as the X of its X-macro:
 * `static const cyphal_port_t ports[] = { R2_HOLO_PORTS(CYPHAL_PORT_ENTRY) };` (holo-player-fw's r2/holo/ports.h) */
#define CYPHAL_PORT_ENTRY(index, kind, name, type, id, priority, id_register, type_register, nvs_key) \
    { CYPHAL_PORT_##kind, name, type, id, CYPHAL_PRIORITY_##priority, id_register, type_register, nvs_key },

/* Whether a port of this kind can be given id: a subject-ID, a service-ID, or CYPHAL_PORT_OFF */
bool cyphal_port_id_valid(cyphal_port_kind_t kind, uint32_t id);

/* The node's own keys in its NVS namespace: no port's nvs_key may be one of them */
#define CYPHAL_NVS_KEY_NODE_ID  "node_id"   /* u8: 0-127, or CYPHAL_NODE_ID_AUTO */
#define CYPHAL_NVS_KEY_NAME     "name"      /* str */
#define CYPHAL_NVS_KEY_BITRATE  "bitrate"   /* u32 */

/* One of the node's own ports, whose ID the standard fixes (the heartbeat, GetInfo, the registers'
 * services...): no application port may have it */
typedef struct {
    cyphal_port_kind_t kind;
    uint16_t id;
} cyphal_fixed_port_t;

/* The node's own ports (kept, not copied), before cyphal_reg_init(), which checks the application's
 * defaults against them; the node gives them from its table of what it serves and publishes */
void cyphal_reg_fixed(const cyphal_fixed_port_t *fixed, size_t count);

/* With CONFIG_CYPHAL_CLIENT, also before cyphal_reg_init(): the standard services the node calls on their fixed IDs,
 * as a test of a service-ID (kept): no CYPHAL_PORT_CLI port of the application's may have one. NULL for none */
void cyphal_reg_called(bool (*fixed)(uint16_t id));

/*
 * The rule for the ports' IDs. Subject-IDs (published and subscribed alike: a subscription on a subject
 * the node publishes would take other nodes' messages of that type for its own) are one set, service-IDs
 * another, and with CONFIG_CYPHAL_CLIENT the service-IDs the node calls a third (its requests and its
 * responses are transfers of different kinds, so it may call a service it also serves: GetInfo, say).
 * Within each, an ID belongs to one port at most: one of the node's own (cyphal_reg_fixed(), and
 * cyphal_reg_called() for the services it calls) or one of the application's. CYPHAL_PORT_OFF is no ID, and any number of ports may be off.
 *
 * Whether the application's port `index` may have `id` beside the other ports' IDs in s, only the first
 * `upto` of them looked at (index itself skipped). False, with why: "subject 2301 is
 * uavcan.pub.event.id's". A write is checked against every port; so a swap goes through a free ID, or off.
 */
bool cyphal_port_id_free(const cyphal_settings_t *s, size_t index, uint32_t id, size_t upto, char *why,
                         size_t why_len);

/* Saved IDs that break that rule, which no write lets in but a configuration from elsewhere can (a backup
 * from another firmware, a port table whose defaults moved): each port, in the table's order, whose ID is
 * one of the node's own or an earlier port's, turned off. A bit (1 << index) for each port turned off */
uint32_t cyphal_ports_settle(cyphal_settings_t *s);

typedef enum {
    CYPHAL_REG_OK,          /* written */
    CYPHAL_REG_IMMUTABLE,   /* a register that can't be written, or no register at that index */
    CYPHAL_REG_BAD_TYPE,    /* a value that isn't (or doesn't convert to) the register's type and size */
    CYPHAL_REG_BAD_VALUE,   /* the right type, out of range, or not a name */
} cyphal_reg_result_t;

/* A register's type, as Access gives it: with cyphal_register_t.count, "natural16[1]" */
typedef enum {
    CYPHAL_REG_STRING,
    CYPHAL_REG_NATURAL16,
    CYPHAL_REG_NATURAL32,
    CYPHAL_REG_BIT,
} cyphal_reg_type_t;

/* cyphal_register_t.flags */
#define CYPHAL_REG_MUTABLE     0x01    /* Access's `mutable`: a write may change it */
#define CYPHAL_REG_PERSISTENT  0x02    /* Access's `persistent`: it keeps its value across a restart */
/* One of the node's settings: kept in cyphal_settings_t, saved by the node, in use from the node's next
 * start. Only the node's own registers; the application's take effect as their write says */
#define CYPHAL_REG_NEXT_START  0x04
/* An application's register whose read may wait (a mutex another task holds through an NVS write): a read over
 * Cyphal is carried out by the node's worker, as a write is, instead of on the node's task. With every job slot
 * held such a read goes unanswered (Access has no way to say busy; counted as busy): the client asks again */
#define CYPHAL_REG_READ_ON_WORKER 0x08

typedef struct cyphal_register cyphal_register_t;
struct cyphal_register {
    const char *name;               /* by uavcan.register.Name's rules: a-z, 0-9, `_`, `.`; one dot at least */
    cyphal_reg_type_t type;
    uint8_t count;                  /* a numeric or bit register's elements, as read: 1 for natural16[1]; 0 for a string */
    uint8_t min_count;              /* the fewest a write may give; 0 for exactly `count` */
    uint8_t flags;                  /* CYPHAL_REG_* */
    /* What a write takes, in the refusal's words, when not the type's own ("one whole number", "2 bits",
     * "a string"): the bitrate's "one or two whole numbers (the second is ignored)". NULL for the type's */
    const char *takes;
    /* Its value into `out`, which comes zeroed. `s` is the node's settings to read a CYPHAL_REG_NEXT_START
     * register from (the saved ones, or those in use); the application's registers ignore it. Runs on the
     * node's task for a read over Cyphal, which must never wait: a copy of a value already in memory,
     * under nothing slower than a spinlock. No NVS, no file, no mutex another task may hold for long; a
     * register whose read needs one is CYPHAL_REG_READ_ON_WORKER, and is read on the node's worker instead.
     * The console's `cyphal register` reads on the console's task */
    void (*read)(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out);
    /* v written, checked first (cyphal_reg_take_*() convert it and check its size): CYPHAL_REG_OK, or a
     * refusal with the reason in `why`, the register unchanged. A CYPHAL_REG_NEXT_START register writes
     * into `s`, which the caller saves; the application's get NULL, keep the value themselves and apply
     * it as they say. Runs on the node's worker task (a write over Cyphal) or the console's, so it may
     * wait, but under the node's lock, which the worker takes too: never for the worker, so never in
     * cyphal_call_wait() (CONFIG_CYPHAL_CLIENT), which refuses there (ESP_ERR_NOT_ALLOWED); cyphal_call()
     * may be used. NULL for a register nobody can write ("it can't be written"); an application's register
     * has one exactly when it is CYPHAL_REG_MUTABLE */
    cyphal_reg_result_t (*write)(const cyphal_register_t *r, cyphal_settings_t *s, const uavcan_register_Value_1_0 *v,
                                 char *why, size_t why_len);
    const void *ctx;                /* the register's own: a port, for the node's port registers */
};

/* The table: the application's ports (kept, not copied: a static table), the name register's name and
 * the application's own registers (kept too; NULL for none), before anything else here but
 * cyphal_reg_fixed(); again replaces them. False, with why, for a table the node can't keep: more than
 * CYPHAL_PORTS_MAX ports, a default ID out of range or taken (cyphal_port_id_free()'s rule), an NVS key
 * too long, two the same or one of the node's own, a register name that isn't one or is taken (the
 * node's, a port's or an application register's, each against all before it), or an application
 * register whose type, size, flags or functions don't fit the rules above. Then the node has none of the
 * application's ports or registers, and the name register is called "node.name". */
bool cyphal_reg_init(const cyphal_port_t *ports, size_t port_count, const char *name_register,
                     const cyphal_register_t *app, size_t app_count, char *why, size_t why_len);
size_t cyphal_port_count(void);
const cyphal_port_t *cyphal_port(size_t index);    /* NULL past the end */
/* Every port's default into s->port_id, and CYPHAL_PORT_OFF in the slots no port has */
void cyphal_ports_default(cyphal_settings_t *s);

size_t cyphal_reg_count(void);
const cyphal_register_t *cyphal_reg(size_t index); /* NULL past the end */
/* The index of the register named by `len` bytes at `name` (not terminated), or -1 for none */
int cyphal_reg_find(const uint8_t *name, size_t len);
const char *cyphal_reg_name(size_t index);         /* NULL past the end */
bool cyphal_reg_mutable(size_t index);
/* Its type in the reference's words, into buf: "natural16[1]", "natural32[2]", "bit[3]" or "string"
 * ("" past the end). buf */
const char *cyphal_reg_type(size_t index, char *buf, size_t len);

/* Its value (from s, for the node's settings); empty past the end (uavcan.register.Access's "no such
 * register") */
void cyphal_reg_read(const cyphal_settings_t *s, size_t index, uavcan_register_Value_1_0 *out);

/* v written by the register's own write (above): into *s for a CYPHAL_REG_NEXT_START register (s may be
 * NULL for any other), *s unchanged unless CYPHAL_REG_OK. Otherwise `why` is the reason, in words for
 * the console ("a node-ID is 0 to 127, or 65535 for plug-and-play"). An empty v is no write, and
 * CYPHAL_REG_BAD_TYPE: Access's read is the caller's. */
cyphal_reg_result_t cyphal_reg_write(cyphal_settings_t *s, size_t index, const uavcan_register_Value_1_0 *v,
                                     char *why, size_t why_len);

/* The console's words as a value of the register's type: one word for a string; for a numeric register
 * each word a decimal number, as natural64, the widest; for a bit register each word 0 or 1. Only the
 * words: how many the register takes, and the numbers' range, are cyphal_reg_write()'s to check.
 * CYPHAL_REG_BAD_TYPE, with why, for a word that isn't one of those (or a string of more than one word,
 * or more words than a value holds); CYPHAL_REG_IMMUTABLE for no register at that index. */
cyphal_reg_result_t cyphal_reg_parse(size_t index, int argc, char *const argv[], uavcan_register_Value_1_0 *out,
                                     char *why, size_t why_len);

/* A value as the console shows it: "52", "1000000 1000000", "1 0 1", the string itself, "(empty)" */
void cyphal_reg_format(const uavcan_register_Value_1_0 *v, char *out, size_t len);

/* ---- for a register's read and write: values to and from its type */

void cyphal_reg_set_string(uavcan_register_Value_1_0 *out, const char *s);
void cyphal_reg_set_natural16(uavcan_register_Value_1_0 *out, const uint16_t *values, size_t count);
void cyphal_reg_set_natural32(uavcan_register_Value_1_0 *out, const uint32_t *values, size_t count);
void cyphal_reg_set_bits(uavcan_register_Value_1_0 *out, const bool *bits, size_t count);

/* An element that is no whole number (a fraction, or beyond what an int64 holds): no range takes it */
#define CYPHAL_REG_NOT_WHOLE INT64_MIN
/* v as r's numbers, into out[0 .. r->count): CYPHAL_REG_BAD_TYPE, with why ("it takes one whole number"),
 * unless v is numeric with r->min_count to r->count elements. Each element as a whole number, or
 * CYPHAL_REG_NOT_WHOLE; those v doesn't have, CYPHAL_REG_NOT_WHOLE too. The range is the caller's */
cyphal_reg_result_t cyphal_reg_take_numbers(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v,
                                            int64_t out[], char *why, size_t why_len);
/* v as r's bits, into out[0 .. r->count): a bit array, or numbers each 0 or 1, of r->min_count to r->count
 * elements; those v doesn't have, false. CYPHAL_REG_BAD_TYPE, or CYPHAL_REG_BAD_VALUE for a number that
 * is neither, with why */
cyphal_reg_result_t cyphal_reg_take_bits(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v, bool out[],
                                         char *why, size_t why_len);
/* v's bytes, for a string register: a string or unstructured bytes, not terminated. CYPHAL_REG_BAD_TYPE,
 * with why, for anything else */
cyphal_reg_result_t cyphal_reg_take_string(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v,
                                           const uint8_t **bytes, size_t *len, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif
