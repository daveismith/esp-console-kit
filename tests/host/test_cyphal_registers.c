/*
 * Host-built tests for cyphal/cyphal_registers.c: the node's register table over its saved settings,
 * with holo-player-fw's port table as that firmware builds it from r2_cyphal_dsdl's ports/holo.csv
 * (its generated r2/holo/ports.h, copied unchanged into fixtures/: a real table of eleven ports of
 * every kind). `make -C tests/host`.
 */
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "cyphal_registers.h"
#include "values.h"
#include "r2/holo/ports.h"

/* The firmware's table, as main/main.c builds it */
static const cyphal_port_t HOLO[] = { R2_HOLO_PORTS(CYPHAL_PORT_ENTRY) };
#define HOLO_N (sizeof(HOLO) / sizeof(HOLO[0]))
_Static_assert(R2_HOLO_PORT_COUNT <= CYPHAL_PORTS_MAX, "the node keeps CYPHAL_PORTS_MAX ports' IDs at most");
#define NAME_REGISTER "r2.node.name"

/* The node's own ports, as cyphal.c gives them from SERVICES[] and plug-and-play's subscription: the
 * heartbeat and the port list, GetInfo, Access, List and ExecuteCommand; NodeIDAllocationData */
static const cyphal_fixed_port_t FIXED[] = {
    { CYPHAL_PORT_PUB, 7509 }, { CYPHAL_PORT_PUB, 7510 }, { CYPHAL_PORT_SRV, 430 }, { CYPHAL_PORT_SRV, 384 },
    { CYPHAL_PORT_SRV, 385 }, { CYPHAL_PORT_SRV, 435 }, { CYPHAL_PORT_SUB, 8166 },
};

/* The register names (holo-player-fw's manual/reference/cyphal.md), in List's order */
static const char *const NAMES[] = {
    "uavcan.node.id", "r2.node.name", "uavcan.can.bitrate",
    "uavcan.pub.status.id", "uavcan.pub.status.type",
    "uavcan.pub.event.id", "uavcan.pub.event.type",
    "uavcan.pub.axes.id", "uavcan.pub.axes.type",
    "uavcan.sub.setpoint.id", "uavcan.sub.setpoint.type",
    "uavcan.srv.motion.id", "uavcan.srv.motion.type",
    "uavcan.srv.light.id", "uavcan.srv.light.type",
    "uavcan.srv.scene.id", "uavcan.srv.scene.type",
    "uavcan.srv.screen.id", "uavcan.srv.screen.type",
    "uavcan.srv.ring.id", "uavcan.srv.ring.type",
    "uavcan.srv.list_scenes.id", "uavcan.srv.list_scenes.type",
    "uavcan.srv.media_info.id", "uavcan.srv.media_info.type",
};
#define NAMES_N (sizeof(NAMES) / sizeof(NAMES[0]))

/* Indexes, by the order above */
enum { NODE_ID, NAME, BITRATE, STATUS_ID, STATUS_TYPE, SETPOINT_ID = 9, SETPOINT_TYPE, MOTION_ID, MOTION_TYPE,
       MEDIA_INFO_ID = 23, MEDIA_INFO_TYPE };

static void defaults(cyphal_settings_t *s)
{
    memset(s, 0xAA, sizeof(*s));    /* nothing left over from before */
    s->node_id = CYPHAL_NODE_ID_AUTO;
    strcpy(s->name, "r2.dome.holo.front");
    s->bitrate = 1000000;
    cyphal_ports_default(s);
}

static int find(const char *name)
{
    return cyphal_reg_find((const uint8_t *)name, strlen(name));
}

static bool is_nat16(const uavcan_register_Value_1_0 *v, size_t count, uint16_t a)
{
    return uavcan_register_Value_1_0_is_natural16_(v) && v->natural16.value.count == count &&
           v->natural16.value.elements[0] == a;
}

static bool is_str(const uavcan_register_Value_1_0 *v, const char *s)
{
    return uavcan_register_Value_1_0_is_string_(v) && v->_string.value.count == strlen(s) &&
           memcmp(v->_string.value.elements, s, strlen(s)) == 0;
}

static void test_table(void)
{
    char why[160];
    CHECK(HOLO_N == R2_HOLO_PORT_COUNT && HOLO_N == 11);
    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(cyphal_port_count() == 11);
    CHECK(cyphal_port(R2_HOLO_PORT_MOTION) == &HOLO[R2_HOLO_PORT_MOTION]);
    CHECK(cyphal_port(11) == NULL);
    CHECK(cyphal_reg_count() == NAMES_N && NAMES_N == 25);
    for (size_t i = 0; i < NAMES_N; i++) {
        CHECK(cyphal_reg_name(i) != NULL && strcmp(cyphal_reg_name(i), NAMES[i]) == 0);
        CHECK(find(NAMES[i]) == (int)i);
    }
    CHECK(cyphal_reg_name(NAMES_N) == NULL);

    /* Names that are nearly one */
    CHECK(find("") == -1);
    CHECK(find("uavcan.node.i") == -1);
    CHECK(find("uavcan.node.idx") == -1);
    CHECK(find("UAVCAN.NODE.ID") == -1);
    CHECK(find("uavcan.node.description") == -1);
    CHECK(find("uavcan.pub.status.id<") == -1);      /* the special registers aren't provided */
    CHECK(cyphal_reg_find((const uint8_t *)"uavcan.node.id\0x", 16) == -1);
    CHECK(cyphal_reg_find((const uint8_t *)"uavcan.node.idx", 14) == NODE_ID);  /* by length, not terminator */

    /* Flags and types: only the .type registers are immutable */
    for (size_t i = 0; i < NAMES_N; i++) {
        const bool type = strstr(NAMES[i], ".type") != NULL;
        CHECK(cyphal_reg_mutable(i) == !type);
        char t[24];
        cyphal_reg_type(i, t, sizeof(t));
        CHECK(strcmp(t, i == BITRATE ? "natural32[2]" : (type || i == NAME) ? "string" : "natural16[1]") == 0);
    }
    CHECK(!cyphal_reg_mutable(NAMES_N));
}

static void test_bad_tables(void)
{
    char why[160];
    cyphal_port_t many[CYPHAL_PORTS_MAX + 1];
    for (size_t i = 0; i < CYPHAL_PORTS_MAX + 1; i++) {
        many[i] = HOLO[0];
    }
    CHECK(!cyphal_reg_init(many, CYPHAL_PORTS_MAX + 1, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(strstr(why, "16") != NULL);
    /* A failed table leaves none, and the name register's own name */
    CHECK(cyphal_port_count() == 0 && cyphal_reg_count() == 3);
    CHECK(strcmp(cyphal_reg_name(NAME), "node.name") == 0);

    cyphal_port_t bad[2] = { HOLO[R2_HOLO_PORT_STATUS], HOLO[R2_HOLO_PORT_MOTION] };
    bad[0].default_id = 8192;
    CHECK(!cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "status") != NULL);
    bad[0].default_id = CYPHAL_PORT_OFF;    /* off by default is allowed */
    CHECK(cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    bad[1].default_id = 512;
    CHECK(!cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "motion") != NULL);
    bad[1].default_id = 100;
    bad[1].nvs_key = "srv.a_long_name_here";
    CHECK(!cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "NVS") != NULL);
    bad[1].nvs_key = bad[0].nvs_key;
    CHECK(!cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "NVS") != NULL);
    bad[1].nvs_key = HOLO[R2_HOLO_PORT_MOTION].nvs_key;
    CHECK(cyphal_reg_init(bad, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(cyphal_reg_count() == 7);

    static const char *const bad_names[] = { "node", "Node.name", "1node.name", ".node.name", "node.name.",
                                             "node..name", "node.na-me", "" };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        CHECK(!cyphal_reg_init(HOLO, HOLO_N, bad_names[i], NULL, 0, why, sizeof(why)));
    }

    /* A register name twice among the node's own: the name register's, and the ports' two each */
    static const char *const taken_names[] = { "uavcan.node.id", "uavcan.can.bitrate", "uavcan.pub.status.id",
                                               "uavcan.srv.media_info.type" };
    for (size_t i = 0; i < sizeof(taken_names) / sizeof(taken_names[0]); i++) {
        CHECK(!cyphal_reg_init(HOLO, HOLO_N, taken_names[i], NULL, 0, why, sizeof(why)));
        CHECK(strstr(why, "twice") != NULL && strstr(why, taken_names[i]) != NULL);
        CHECK(cyphal_port_count() == 0 && cyphal_reg_count() == 3);
    }
    cyphal_port_t dup[2] = { HOLO[R2_HOLO_PORT_STATUS], HOLO[R2_HOLO_PORT_EVENT] };
    dup[1].id_register = dup[0].id_register;     /* two ports' .id the same */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "twice") != NULL);
    dup[1] = HOLO[R2_HOLO_PORT_EVENT];
    dup[1].type_register = dup[0].id_register;   /* one's .type another's .id */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "twice") != NULL);
    dup[1] = HOLO[R2_HOLO_PORT_EVENT];
    dup[1].type_register = dup[1].id_register;   /* its own two */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "twice") != NULL);
    dup[1] = HOLO[R2_HOLO_PORT_EVENT];
    dup[1].id_register = "uavcan.node.id";       /* the node's */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)) && strstr(why, "twice") != NULL);

    /* An NVS key that is one of the node's own */
    static const char *const node_keys[] = { CYPHAL_NVS_KEY_NODE_ID, CYPHAL_NVS_KEY_NAME, CYPHAL_NVS_KEY_BITRATE };
    for (size_t i = 0; i < 3; i++) {
        dup[1] = HOLO[R2_HOLO_PORT_EVENT];
        dup[1].nvs_key = node_keys[i];
        CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
        CHECK(strstr(why, "one of the node's own") != NULL && strstr(why, node_keys[i]) != NULL);
    }

    /* Defaults by the port rule: not one of the node's own ports, and not another port's, subjects
     * (published or subscribed) and services each a set of their own */
    dup[1] = HOLO[R2_HOLO_PORT_EVENT];
    dup[1].default_id = 7509;                    /* the heartbeat */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(strstr(why, "event") != NULL && strstr(why, "subject 7509 is one of the node's own ports") != NULL);
    dup[1].default_id = 8166;                    /* plug-and-play's, subscribed */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    dup[1].default_id = 2300;                    /* status's */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(strstr(why, "subject 2300 is uavcan.pub.status.id's") != NULL);
    dup[1] = HOLO[R2_HOLO_PORT_SETPOINT];
    dup[1].default_id = 2300;                    /* a subscription on a subject published */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    dup[1] = HOLO[R2_HOLO_PORT_MOTION];
    dup[1].default_id = 435;                     /* ExecuteCommand */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(strstr(why, "service 435 is one of the node's own ports") != NULL);
    dup[1].default_id = 2300;                    /* a service-ID out of range anyway */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    dup[0] = HOLO[R2_HOLO_PORT_LIGHT];
    dup[1].default_id = 101;                     /* light's */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    CHECK(strstr(why, "service 101 is uavcan.srv.light.id's") != NULL);
    dup[0] = HOLO[R2_HOLO_PORT_STATUS];
    dup[0].default_id = 384;                     /* a subject 384 is no service: free */
    dup[1].default_id = 7509;                    /* nor a service 7509 a subject: out of range, though */
    CHECK(!cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    dup[1].default_id = 100;
    CHECK(cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));
    dup[0].default_id = dup[1].default_id = CYPHAL_PORT_OFF;    /* off is no ID: any number of ports */
    dup[1].kind = CYPHAL_PORT_PUB;
    CHECK(cyphal_reg_init(dup, 2, NAME_REGISTER, NULL, 0, why, sizeof(why)));

    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, NULL, 0, why, sizeof(why)));
}

static void test_read_defaults(void)
{
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v;

    cyphal_reg_read(&s, NODE_ID, &v);
    CHECK(is_nat16(&v, 1, 65535));      /* plug-and-play reads as the standard's "unset" */
    cyphal_reg_read(&s, NAME, &v);
    CHECK(is_str(&v, "r2.dome.holo.front"));
    cyphal_reg_read(&s, BITRATE, &v);
    CHECK(uavcan_register_Value_1_0_is_natural32_(&v) && v.natural32.value.count == 2 &&
          v.natural32.value.elements[0] == 1000000 && v.natural32.value.elements[1] == 1000000);
    /* Each port's default and type, from the table */
    for (size_t p = 0; p < HOLO_N; p++) {
        cyphal_reg_read(&s, 3 + 2 * p, &v);
        CHECK(is_nat16(&v, 1, HOLO[p].default_id));
        cyphal_reg_read(&s, 4 + 2 * p, &v);
        CHECK(is_str(&v, HOLO[p].type));
    }
    cyphal_reg_read(&s, STATUS_ID, &v);
    CHECK(is_nat16(&v, 1, 2300));
    cyphal_reg_read(&s, MOTION_ID, &v);
    CHECK(is_nat16(&v, 1, 100));
    cyphal_reg_read(&s, MOTION_TYPE, &v);
    CHECK(is_str(&v, "r2.holo.Motion.0.1"));
    cyphal_reg_read(&s, SETPOINT_ID, &v);
    CHECK(is_nat16(&v, 1, 2303));
    cyphal_reg_read(&s, MEDIA_INFO_TYPE, &v);
    CHECK(is_str(&v, "r2.holo.MediaInfo.0.1"));
    /* The unused slots are off */
    for (size_t p = HOLO_N; p < CYPHAL_PORTS_MAX; p++) {
        CHECK(s.port_id[p] == CYPHAL_PORT_OFF);
    }
    /* Past the end: empty, as for a register that doesn't exist */
    cyphal_reg_read(&s, NAMES_N, &v);
    CHECK(uavcan_register_Value_1_0_is_empty_(&v));
}

/* A write refused: the settings untouched, and the reason given */
static void refused(size_t index, const uavcan_register_Value_1_0 *v, cyphal_reg_result_t want, const char *reason)
{
    cyphal_settings_t s, before;
    defaults(&s);
    before = s;
    char why[160] = "";
    const cyphal_reg_result_t r = cyphal_reg_write(&s, index, v, why, sizeof(why));
    CHECK(r == want);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    if (strstr(why, reason) == NULL) {
        printf("  register %zu: why is \"%s\", not containing \"%s\"\n", index, why, reason);
    }
    CHECK(strstr(why, reason) != NULL);
}

static void test_node_id(void)
{
    char why[160];
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v = nat16(1, 52), r;
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s.node_id == 52);
    cyphal_reg_read(&s, NODE_ID, &r);
    CHECK(is_nat16(&r, 1, 52));
    v = nat16(1, 0);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 0);
    v = nat16(1, 127);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 127);
    v = nat16(1, 65535);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 255);

    /* Any numeric type converts, when the number is whole */
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_natural8_(&v);
    v.natural8.value.elements[0] = 53;
    v.natural8.value.count = 1;
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 53);
    uavcan_register_Value_1_0_select_integer64_(&v);
    v.integer64.value.elements[0] = 54;
    v.integer64.value.count = 1;
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 54);
    v = nat32(1, 55, 0, 0);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 55);
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_real32_(&v);
    v.real32.value.elements[0] = 56.0f;
    v.real32.value.count = 1;
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 56);

    /* Out of range, or not whole */
    const char *range = "a node-ID is 0 to 127, or 65535 for plug-and-play";
    v = nat16(1, 128);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);
    v = nat16(1, 65534);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);
    v = nat32(1, 65536 + 52, 0, 0);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);
    v.real32.value.elements[0] = 52.5f;
    uavcan_register_Value_1_0_select_real32_(&v);
    v.real32.value.count = 1;
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_integer8_(&v);
    v.integer8.value.elements[0] = -1;
    v.integer8.value.count = 1;
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_natural64_(&v);
    v.natural64.value.elements[0] = 0xFFFFFFFFFFFFFFFFull;
    v.natural64.value.count = 1;
    refused(NODE_ID, &v, CYPHAL_REG_BAD_VALUE, range);

    /* Not one number */
    v = nat16(2, 52, 52);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");
    v = nat16(0);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");
    v = str("52", 2);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_bit_(&v);
    v.bit.value.count = 1;
    refused(NODE_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");
    uavcan_register_Value_1_0_select_empty_(&v);
    refused(NODE_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");
}

static void test_name(void)
{
    char why[160];
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v = str("r2.dome.holo.top", 16), r;
    CHECK(cyphal_reg_write(&s, NAME, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(strcmp(s.name, "r2.dome.holo.top") == 0);
    cyphal_reg_read(&s, NAME, &r);
    CHECK(is_str(&r, "r2.dome.holo.top"));
    /* Bytes do as well as a string */
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_unstructured_(&v);
    memcpy(v.unstructured.value.elements, "r2.x", 4);
    v.unstructured.value.count = 4;
    CHECK(cyphal_reg_write(&s, NAME, &v, why, sizeof(why)) == CYPHAL_REG_OK && strcmp(s.name, "r2.x") == 0);
    /* The full 50 */
    char fifty[51];
    memset(fifty, 'a', 50);
    fifty[50] = '\0';
    v = str(fifty, 50);
    CHECK(cyphal_reg_write(&s, NAME, &v, why, sizeof(why)) == CYPHAL_REG_OK && strcmp(s.name, fifty) == 0);

    /* `cyphal name`'s reasons, word for word */
    v = str("R2.Dome", 7);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "`R` is not allowed");
    v = str("r2..holo", 8);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "a part between dots is empty");
    v = str("r2.1holo", 8);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "can't start with a digit: `1holo`");
    v = str("", 0);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "1 to 50 characters, and this is 0");
    char long_name[200];
    memset(long_name, 'a', sizeof(long_name));
    v = str(long_name, 51);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "and this is 51");
    v = str(long_name, 200);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "and this is 200");
    /* A NUL inside, which a C string would cut short to a good name */
    v = str("r2.x\0y", 6);
    refused(NAME, &v, CYPHAL_REG_BAD_VALUE, "0x00 is not allowed");
    v = nat16(1, 5);
    refused(NAME, &v, CYPHAL_REG_BAD_TYPE, "a string");
}

static void test_bitrate(void)
{
    char why[160];
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v = nat32(2, 500000, 500000, 0), r;
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.bitrate == 500000);
    /* One value alone, and a second that differs: the first is the bitrate, and is what reads back twice */
    v = nat32(1, 250000, 0, 0);
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.bitrate == 250000);
    v = nat32(2, 125000, 4000000, 0);
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.bitrate == 125000);
    cyphal_reg_read(&s, BITRATE, &r);
    CHECK(r.natural32.value.count == 2 && r.natural32.value.elements[0] == 125000 &&
          r.natural32.value.elements[1] == 125000);
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_real64_(&v);
    v.real64.value.elements[0] = 1e6;
    v.real64.value.count = 1;
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.bitrate == 1000000);

    const char *rates = "the bitrate is 125000, 250000, 500000 or 1000000";
    v = nat32(2, 123456, 123456, 0);
    refused(BITRATE, &v, CYPHAL_REG_BAD_VALUE, rates);
    v = nat32(1, 0, 0, 0);
    refused(BITRATE, &v, CYPHAL_REG_BAD_VALUE, rates);
    v = nat16(1, 50000);
    refused(BITRATE, &v, CYPHAL_REG_BAD_VALUE, rates);
    v = nat32(3, 1000000, 1000000, 1000000);
    refused(BITRATE, &v, CYPHAL_REG_BAD_TYPE, "one or two whole numbers");
    v = str("1m", 2);
    refused(BITRATE, &v, CYPHAL_REG_BAD_TYPE, "one or two whole numbers");
}

static void test_ports(void)
{
    char why[160];
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v = nat16(1, 2310), r;
    CHECK(cyphal_reg_write(&s, STATUS_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s.port_id[R2_HOLO_PORT_STATUS] == 2310);
    cyphal_reg_read(&s, STATUS_ID, &r);
    CHECK(is_nat16(&r, 1, 2310));
    /* Nothing else moved */
    CHECK(s.port_id[R2_HOLO_PORT_EVENT] == 2301 && s.port_id[R2_HOLO_PORT_MOTION] == 100);
    v = nat16(1, 8191);
    CHECK(cyphal_reg_write(&s, SETPOINT_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 0);
    CHECK(cyphal_reg_write(&s, STATUS_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[0] == 0);
    v = nat16(1, 65535);
    CHECK(cyphal_reg_write(&s, STATUS_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[0] == 65535);
    v = nat16(1, 511);
    CHECK(cyphal_reg_write(&s, MOTION_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 65535);
    CHECK(cyphal_reg_write(&s, MEDIA_INFO_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s.port_id[R2_HOLO_PORT_MEDIA_INFO] == 65535);

    v = nat16(1, 8192);
    refused(STATUS_ID, &v, CYPHAL_REG_BAD_VALUE, "a subject-ID is 0 to 8191, or 65535 to turn the port off");
    refused(SETPOINT_ID, &v, CYPHAL_REG_BAD_VALUE, "a subject-ID is 0 to 8191");
    v = nat16(1, 512);
    refused(MOTION_ID, &v, CYPHAL_REG_BAD_VALUE, "a service-ID is 0 to 511, or 65535 to turn the port off");
    v = nat16(1, 65534);
    refused(MEDIA_INFO_ID, &v, CYPHAL_REG_BAD_VALUE, "a service-ID is 0 to 511");
    v = nat16(2, 1, 2);
    refused(MOTION_ID, &v, CYPHAL_REG_BAD_TYPE, "one whole number");

    /* The .type registers can't be written, whatever the value; nor can a register that isn't there */
    v = str("r2.holo.Motion.0.2", 18);
    refused(MOTION_TYPE, &v, CYPHAL_REG_IMMUTABLE, "can't be written");
    v = nat16(1, 1);
    refused(STATUS_TYPE, &v, CYPHAL_REG_IMMUTABLE, "can't be written");
    refused(NAMES_N, &v, CYPHAL_REG_IMMUTABLE, "no register");

    /* The port rule: not one of the node's own ports, not another port's; subjects and services apart */
    v = nat16(1, 7509);
    refused(STATUS_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 7509 is one of the node's own ports");
    v = nat16(1, 7510);
    refused(SETPOINT_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 7510 is one of the node's own ports");
    v = nat16(1, 8166);
    refused(STATUS_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 8166 is one of the node's own ports");
    static const uint16_t own_services[] = { 384, 385, 430, 435 };
    for (size_t i = 0; i < 4; i++) {
        v = nat16(1, own_services[i]);
        refused(MOTION_ID, &v, CYPHAL_REG_BAD_VALUE, "is one of the node's own ports");
    }
    v = nat16(1, 2301);
    refused(STATUS_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 2301 is uavcan.pub.event.id's");
    v = nat16(1, 2300);
    refused(SETPOINT_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 2300 is uavcan.pub.status.id's");     /* sub on a pub */
    v = nat16(1, 2303);
    refused(STATUS_ID, &v, CYPHAL_REG_BAD_VALUE, "subject 2303 is uavcan.sub.setpoint.id's");    /* and the reverse */
    v = nat16(1, 106);
    refused(MOTION_ID, &v, CYPHAL_REG_BAD_VALUE, "service 106 is uavcan.srv.media_info.id's");
    /* Its own ID again; a subject that is a service's number; and 384 as a subject */
    defaults(&s);
    v = nat16(1, 2300);
    CHECK(cyphal_reg_write(&s, STATUS_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[0] == 2300);
    v = nat16(1, 100);
    CHECK(cyphal_reg_write(&s, STATUS_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.port_id[0] == 100);
    v = nat16(1, 384);
    CHECK(cyphal_reg_write(&s, SETPOINT_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    /* A swap goes through off: then any number of ports may be off together */
    defaults(&s);
    v = nat16(1, 65535);
    CHECK(cyphal_reg_write(&s, MOTION_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, MEDIA_INFO_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 100);
    CHECK(cyphal_reg_write(&s, MEDIA_INFO_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    v = nat16(1, 106);
    CHECK(cyphal_reg_write(&s, MOTION_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s.port_id[R2_HOLO_PORT_MOTION] == 106 && s.port_id[R2_HOLO_PORT_MEDIA_INFO] == 100);
    /* cyphal_port_id_free() itself, by `upto` */
    defaults(&s);
    CHECK(!cyphal_port_id_free(&s, R2_HOLO_PORT_EVENT, 2300, HOLO_N, why, sizeof(why)));
    CHECK(cyphal_port_id_free(&s, R2_HOLO_PORT_STATUS, 2301, 1, why, sizeof(why)));     /* event not looked at */
    CHECK(!cyphal_port_id_free(&s, R2_HOLO_PORT_STATUS, 2301, 2, why, sizeof(why)));
    CHECK(cyphal_port_id_free(&s, R2_HOLO_PORT_STATUS, CYPHAL_PORT_OFF, HOLO_N, why, sizeof(why)));
    CHECK(!cyphal_port_id_free(&s, R2_HOLO_PORT_STATUS, 7509, 0, why, sizeof(why)));    /* the node's, always */

    CHECK(cyphal_port_id_valid(CYPHAL_PORT_PUB, 0) && cyphal_port_id_valid(CYPHAL_PORT_SUB, 8191));
    CHECK(!cyphal_port_id_valid(CYPHAL_PORT_PUB, 8192) && cyphal_port_id_valid(CYPHAL_PORT_PUB, 65535));
    CHECK(cyphal_port_id_valid(CYPHAL_PORT_SRV, 511) && !cyphal_port_id_valid(CYPHAL_PORT_SRV, 512));
    CHECK(cyphal_port_id_valid(CYPHAL_PORT_SRV, 65535) && !cyphal_port_id_valid(CYPHAL_PORT_SRV, 65536));
}

static void test_parse_format(void)
{
    char why[160], text[300];
    uavcan_register_Value_1_0 v;
    char *one[] = { "52" }, *two[] = { "500000", "1000000" }, *three[] = { "1", "2", "3" };
    char *word[] = { "auto" }, *neg[] = { "-1" }, *huge[] = { "99999999999999999999999" }, *big[] = { "70000" };
    char *name[] = { "r2.dome.holo.top" };

    CHECK(cyphal_reg_parse(NODE_ID, 1, one, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    cyphal_settings_t s;
    defaults(&s);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.node_id == 52);
    cyphal_reg_format(&v, text, sizeof(text));
    CHECK(strcmp(text, "52") == 0);
    /* A number too big for the register's type parses, and its range is the write's: the same message
     * as over Cyphal */
    CHECK(cyphal_reg_parse(NODE_ID, 1, big, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);
    CHECK(strstr(why, "a node-ID is 0 to 127") != NULL);
    /* Parsing takes any number of words; how many the register takes is the write's rule, and message */
    CHECK(cyphal_reg_parse(NODE_ID, 2, two, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes one whole number") == 0);
    CHECK(cyphal_reg_parse(NODE_ID, 0, one, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, NODE_ID, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    /* More words than a value holds: refused here, in the write's words */
    char *many[33];
    for (size_t i = 0; i < 33; i++) {
        many[i] = one[0];
    }
    CHECK(cyphal_reg_parse(NODE_ID, 33, many, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes one whole number") == 0);
    CHECK(cyphal_reg_parse(NODE_ID, 1, word, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strstr(why, "`auto`") != NULL);
    CHECK(cyphal_reg_parse(NODE_ID, 1, neg, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(cyphal_reg_parse(NODE_ID, 1, huge, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);

    CHECK(cyphal_reg_parse(BITRATE, 2, two, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_OK && s.bitrate == 500000);
    cyphal_reg_format(&v, text, sizeof(text));
    CHECK(strcmp(text, "500000 1000000") == 0);
    CHECK(cyphal_reg_parse(BITRATE, 1, two, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_parse(BITRATE, 3, three, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, BITRATE, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes one or two whole numbers (the second is ignored)") == 0);

    CHECK(cyphal_reg_parse(NAME, 1, name, &v, why, sizeof(why)) == CYPHAL_REG_OK && is_str(&v, name[0]));
    CHECK(cyphal_reg_parse(NAME, 2, two, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes a string, one word") == 0);
    CHECK(cyphal_reg_parse(STATUS_ID, 1, one, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    /* A .type register parses (it is a string) and the write refuses it */
    CHECK(cyphal_reg_parse(STATUS_TYPE, 1, name, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(&s, STATUS_TYPE, &v, why, sizeof(why)) == CYPHAL_REG_IMMUTABLE);

    /* What reads come back as */
    cyphal_reg_read(&s, STATUS_TYPE, &v);
    cyphal_reg_format(&v, text, sizeof(text));
    CHECK(strcmp(text, "r2.holo.Status.0.1") == 0);
    cyphal_reg_read(&s, BITRATE, &v);
    cyphal_reg_format(&v, text, sizeof(text));
    CHECK(strcmp(text, "500000 500000") == 0);
    uavcan_register_Value_1_0_select_empty_(&v);
    cyphal_reg_format(&v, text, sizeof(text));
    CHECK(strcmp(text, "(empty)") == 0);
    /* Cut short, but terminated */
    cyphal_reg_read(&s, MEDIA_INFO_TYPE, &v);
    cyphal_reg_format(&v, text, 8);
    CHECK(strcmp(text, "r2.holo") == 0);
}

/* ---- the application's own registers: a fake pair of endpoints (natural16[2]) and a bit[3], kept by
 * "the application" and in effect at once, and a read-only one */

static uint16_t s_endpoints[2] = { 1000, 2000 };
static bool s_release[3] = { true, false, true };
static int s_endpoint_writes;

static void read_endpoints(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    (void)s;
    cyphal_reg_set_natural16(out, s_endpoints, 2);
}

static cyphal_reg_result_t write_endpoints(const cyphal_register_t *r, cyphal_settings_t *s,
                                           const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    CHECK(s == NULL);
    int64_t n[2];
    const cyphal_reg_result_t res = cyphal_reg_take_numbers(r, v, n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    if (n[0] < 500 || n[0] > 2500 || n[1] < 500 || n[1] > 2500 || n[0] >= n[1]) {
        snprintf(why, why_len, "endpoints are 500 to 2500 us, the first below the second");
        return CYPHAL_REG_BAD_VALUE;
    }
    s_endpoints[0] = (uint16_t)n[0];
    s_endpoints[1] = (uint16_t)n[1];
    s_endpoint_writes++;
    return CYPHAL_REG_OK;
}

static void read_release(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    (void)s;
    cyphal_reg_set_bits(out, s_release, 3);
}

static cyphal_reg_result_t write_release(const cyphal_register_t *r, cyphal_settings_t *s,
                                         const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    (void)s;
    bool b[3];
    const cyphal_reg_result_t res = cyphal_reg_take_bits(r, v, b, why, why_len);
    if (res == CYPHAL_REG_OK) {
        memcpy(s_release, b, sizeof(b));
    }
    return res;
}

static void read_version(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    (void)s;
    cyphal_reg_set_string(out, "v1.2.3");
}

static const cyphal_register_t APP[] = {
    { "r2.test.endpoints_us", CYPHAL_REG_NATURAL16, 2, 0, CYPHAL_REG_MUTABLE | CYPHAL_REG_PERSISTENT, NULL,
      read_endpoints, write_endpoints, NULL },
    { "r2.test.release", CYPHAL_REG_BIT, 3, 0, CYPHAL_REG_MUTABLE, NULL, read_release, write_release, NULL },
    { "r2.test.version", CYPHAL_REG_STRING, 0, 0, 0, NULL, read_version, NULL, NULL },
};
#define APP_N (sizeof(APP) / sizeof(APP[0]))
enum { ENDPOINTS = NAMES_N, RELEASE, VERSION };

static bool is_bits(const uavcan_register_Value_1_0 *v, bool a, bool b, bool c)
{
    return uavcan_register_Value_1_0_is_bit_(v) && v->bit.value.count == 3 &&
           v->bit.value.bitpacked[0] == (uint8_t)(a | b << 1 | c << 2);
}

static void test_app_registers(void)
{
    char why[160], text[64], type[24];
    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, APP, APP_N, why, sizeof(why)));
    /* After the node's own, in the application's order; the node's unmoved */
    CHECK(cyphal_reg_count() == NAMES_N + APP_N);
    for (size_t i = 0; i < NAMES_N; i++) {
        CHECK(strcmp(cyphal_reg_name(i), NAMES[i]) == 0);
    }
    CHECK(find("r2.test.endpoints_us") == ENDPOINTS && find("r2.test.version") == VERSION);
    CHECK(cyphal_reg(ENDPOINTS) == &APP[0] && cyphal_reg(VERSION + 1) == NULL);
    CHECK(strcmp(cyphal_reg_type(ENDPOINTS, type, sizeof(type)), "natural16[2]") == 0);
    CHECK(strcmp(cyphal_reg_type(RELEASE, type, sizeof(type)), "bit[3]") == 0);
    CHECK(strcmp(cyphal_reg_type(VERSION, type, sizeof(type)), "string") == 0);
    CHECK(strcmp(cyphal_reg_type(VERSION + 1, type, sizeof(type)), "") == 0);
    CHECK(cyphal_reg_mutable(ENDPOINTS) && cyphal_reg_mutable(RELEASE) && !cyphal_reg_mutable(VERSION));
    /* The flags are each register's: the node's settings apply from its next start, the application's not */
    CHECK(cyphal_reg(NODE_ID)->flags & CYPHAL_REG_NEXT_START && cyphal_reg(STATUS_ID)->flags & CYPHAL_REG_NEXT_START);
    CHECK(!(cyphal_reg(STATUS_TYPE)->flags & (CYPHAL_REG_NEXT_START | CYPHAL_REG_MUTABLE)));
    CHECK(cyphal_reg(STATUS_TYPE)->flags & CYPHAL_REG_PERSISTENT);
    CHECK(!(cyphal_reg(RELEASE)->flags & (CYPHAL_REG_NEXT_START | CYPHAL_REG_PERSISTENT)));

    /* Read: from the application's store, whatever settings are given */
    cyphal_settings_t s;
    defaults(&s);
    uavcan_register_Value_1_0 v, r;
    cyphal_reg_read(&s, ENDPOINTS, &r);
    CHECK(uavcan_register_Value_1_0_is_natural16_(&r) && r.natural16.value.count == 2 &&
          r.natural16.value.elements[0] == 1000 && r.natural16.value.elements[1] == 2000);
    cyphal_reg_format(&r, text, sizeof(text));
    CHECK(strcmp(text, "1000 2000") == 0);
    cyphal_reg_read(&s, RELEASE, &r);
    CHECK(is_bits(&r, true, false, true));
    cyphal_reg_format(&r, text, sizeof(text));
    CHECK(strcmp(text, "1 0 1") == 0);
    cyphal_reg_read(&s, VERSION, &r);
    CHECK(is_str(&r, "v1.2.3"));

    /* Write: into the application's store, at once, with no settings */
    v = nat16(2, 1100, 1900);
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s_endpoints[0] == 1100 && s_endpoints[1] == 1900 && s_endpoint_writes == 1);
    v = nat32(2, 1200, 1800, 0);    /* another width converts */
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_OK && s_endpoints[0] == 1200);
    v = nat16(1, 1100);
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes 2 whole numbers") == 0);
    v = nat16(2, 1900, 1100);
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);
    CHECK(strstr(why, "first below the second") != NULL && s_endpoints[0] == 1200 && s_endpoint_writes == 2);
    memset(&v, 0, sizeof(v));
    uavcan_register_Value_1_0_select_real32_(&v);
    v.real32.value.elements[0] = 1000.5f;      /* not whole: out of every range */
    v.real32.value.elements[1] = 2000.0f;
    v.real32.value.count = 2;
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);

    /* Bits: a bit array, or numbers that are each 0 or 1 */
    bool bits[3] = { false, true, true };
    cyphal_reg_set_bits(&v, bits, 3);
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(!s_release[0] && s_release[1] && s_release[2]);
    v = nat16(3, 1, 0, 0);
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(s_release[0] && !s_release[1] && !s_release[2]);
    v = nat16(3, 1, 2, 0);
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_BAD_VALUE);
    CHECK(strcmp(why, "a bit is 0 or 1") == 0 && s_release[0] && !s_release[1]);
    cyphal_reg_set_bits(&v, bits, 2);
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "it takes 3 bits") == 0);
    v = str("101", 3);
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);

    /* The console's words for each */
    char *on_off[] = { "0", "1", "1" }, *bad_bit[] = { "0", "2", "1" }, *pair[] = { "1300", "1700" };
    CHECK(cyphal_reg_parse(RELEASE, 3, on_off, &v, why, sizeof(why)) == CYPHAL_REG_OK && is_bits(&v, false, true, true));
    CHECK(cyphal_reg_write(NULL, RELEASE, &v, why, sizeof(why)) == CYPHAL_REG_OK && !s_release[0] && s_release[2]);
    CHECK(cyphal_reg_parse(RELEASE, 3, bad_bit, &v, why, sizeof(why)) == CYPHAL_REG_BAD_TYPE);
    CHECK(strcmp(why, "`2` is not 0 or 1") == 0);
    CHECK(cyphal_reg_parse(ENDPOINTS, 2, pair, &v, why, sizeof(why)) == CYPHAL_REG_OK);
    CHECK(cyphal_reg_write(NULL, ENDPOINTS, &v, why, sizeof(why)) == CYPHAL_REG_OK && s_endpoints[1] == 1700);

    /* A register with no write can't be written */
    v = str("v2", 2);
    CHECK(cyphal_reg_write(NULL, VERSION, &v, why, sizeof(why)) == CYPHAL_REG_IMMUTABLE);
    CHECK(strcmp(why, "it can't be written") == 0);

    /* Tables the node refuses: then none of the application's, ports or registers */
    cyphal_register_t bad[2] = { APP[0], APP[1] };
    bad[1].name = APP[0].name;
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)) && strstr(why, "taken") != NULL);
    CHECK(cyphal_reg_count() == 3 && cyphal_port_count() == 0);
    bad[1].name = "uavcan.node.id";     /* the node's own */
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1].name = "uavcan.srv.motion.id";
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1] = APP[1];
    bad[1].flags |= CYPHAL_REG_NEXT_START;      /* only the node's settings */
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)) && strstr(why, "fit") != NULL);
    bad[1] = APP[1];
    bad[1].write = NULL;                        /* mutable, with nothing to write it */
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1] = APP[1];
    bad[1].count = 0;
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1] = APP[1];
    bad[1].read = NULL;
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1] = APP[1];
    bad[1].name = "R2.release";
    CHECK(!cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)));
    bad[1] = APP[1];
    bad[1].flags |= CYPHAL_REG_READ_ON_WORKER;  /* an application's read that may wait: taken, the flag kept */
    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)) && cyphal_reg_count() == NAMES_N + 2);
    CHECK(cyphal_reg(NAMES_N + 1)->flags & CYPHAL_REG_READ_ON_WORKER);
    bad[1] = APP[1];
    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, bad, 2, why, sizeof(why)) && cyphal_reg_count() == NAMES_N + 2);
    CHECK(cyphal_reg_init(HOLO, HOLO_N, NAME_REGISTER, NULL, 0, why, sizeof(why)));
}

/* Saved IDs that break the rule (from elsewhere), as the node loads them: the later port off */
static void test_settle(void)
{
    cyphal_settings_t s;
    defaults(&s);
    CHECK(cyphal_ports_settle(&s) == 0);    /* the defaults keep the rule */
    cyphal_settings_t before = s;
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);

    s.port_id[R2_HOLO_PORT_STATUS] = 7509;              /* the heartbeat's */
    s.port_id[R2_HOLO_PORT_AXES] = 2301;                /* event's, which comes first */
    s.port_id[R2_HOLO_PORT_SETPOINT] = 2300;            /* status's, but status is off by now */
    s.port_id[R2_HOLO_PORT_RING] = 435;                 /* ExecuteCommand */
    s.port_id[R2_HOLO_PORT_MEDIA_INFO] = 101;           /* light's */
    s.port_id[R2_HOLO_PORT_LIST_SCENES] = CYPHAL_PORT_OFF;
    s.port_id[R2_HOLO_PORT_SCREEN] = CYPHAL_PORT_OFF;   /* two off: kept so */
    const uint32_t off = cyphal_ports_settle(&s);
    CHECK(off == (1u << R2_HOLO_PORT_STATUS | 1u << R2_HOLO_PORT_AXES | 1u << R2_HOLO_PORT_RING |
                  1u << R2_HOLO_PORT_MEDIA_INFO));
    CHECK(s.port_id[R2_HOLO_PORT_STATUS] == CYPHAL_PORT_OFF && s.port_id[R2_HOLO_PORT_AXES] == CYPHAL_PORT_OFF);
    CHECK(s.port_id[R2_HOLO_PORT_SETPOINT] == 2300 && s.port_id[R2_HOLO_PORT_EVENT] == 2301);
    CHECK(s.port_id[R2_HOLO_PORT_RING] == CYPHAL_PORT_OFF && s.port_id[R2_HOLO_PORT_MEDIA_INFO] == CYPHAL_PORT_OFF);
    CHECK(s.port_id[R2_HOLO_PORT_LIGHT] == 101 && s.port_id[R2_HOLO_PORT_MOTION] == 100);
    CHECK(s.port_id[R2_HOLO_PORT_SCREEN] == CYPHAL_PORT_OFF);
    CHECK(cyphal_ports_settle(&s) == 0);    /* settled */
    /* The other settings untouched */
    CHECK(s.node_id == before.node_id && s.bitrate == before.bitrate && strcmp(s.name, before.name) == 0);
}

static void test_settings(void)
{
    cyphal_settings_t a, b;
    defaults(&a);
    b = a;
    CHECK(cyphal_settings_equal(&a, &b));
    b.port_id[R2_HOLO_PORT_RING] = 200;
    CHECK(!cyphal_settings_equal(&a, &b));
    CHECK(cyphal_bitrate_valid(125000) && cyphal_bitrate_valid(1000000) && !cyphal_bitrate_valid(800000));
    CHECK(!cyphal_bitrate_valid(0));
    CHECK(cyphal_node_id_valid(0) && cyphal_node_id_valid(CYPHAL_NODE_ID_MAX) && cyphal_node_id_valid(CYPHAL_NODE_ID_AUTO));
    CHECK(!cyphal_node_id_valid(CYPHAL_NODE_ID_MAX + 1) && !cyphal_node_id_valid(254) && !cyphal_node_id_valid(65535));
}

int main(void)
{
    cyphal_reg_fixed(FIXED, sizeof(FIXED) / sizeof(FIXED[0]));
    test_table();
    test_bad_tables();
    test_read_defaults();
    test_node_id();
    test_name();
    test_bitrate();
    test_ports();
    test_parse_format();
    test_app_registers();
    test_settle();
    test_settings();
    printf("cyphal_registers: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
