/*
 * See cyphal_registers.h. Plain C only: tests/host builds this file on the computer.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cyphal_registers.h"

#define NODE_REGISTERS  3       /* the node-ID, the name, the bitrate; then two for each port */

/* The node's own registers, built by cyphal_reg_init(): NODE_REGISTERS, then each port's .id and .type */
static cyphal_register_t s_own[NODE_REGISTERS + 2 * CYPHAL_PORTS_MAX];
static size_t s_own_count;
static const cyphal_port_t *s_ports;
static size_t s_port_count;
static const cyphal_register_t *s_app;  /* the application's, after s_own */
static size_t s_app_count;
static const cyphal_fixed_port_t *s_fixed;     /* the node's own ports (cyphal_reg_fixed()) */
static size_t s_fixed_count;

bool cyphal_port_id_valid(cyphal_port_kind_t kind, uint32_t id)
{
    return id == CYPHAL_PORT_OFF || id <= (kind == CYPHAL_PORT_SRV ? CYPHAL_SERVICE_ID_MAX : CYPHAL_SUBJECT_ID_MAX);
}

void cyphal_reg_fixed(const cyphal_fixed_port_t *fixed, size_t count)
{
    s_fixed = fixed;
    s_fixed_count = fixed != NULL ? count : 0;
}

/* cyphal_port_id_free()'s rule over a table and its IDs: whether `id`, for a port of `kind` at `index`, is
 * one of the node's own ports' or one of ports[0 .. upto)'s in ids[] (index skipped). True, with why */
static bool id_taken(const cyphal_port_t *ports, const uint16_t *ids, size_t upto, size_t index,
                     cyphal_port_kind_t kind, uint32_t id, char *why, size_t why_len)
{
    if (id == CYPHAL_PORT_OFF) {
        return false;
    }
    const bool service = kind == CYPHAL_PORT_SRV;
    const char *what = service ? "service" : "subject";
    for (size_t i = 0; i < s_fixed_count; i++) {
        if ((s_fixed[i].kind == CYPHAL_PORT_SRV) == service && s_fixed[i].id == id) {
            snprintf(why, why_len, "%s %u is one of the node's own ports (uavcan.node.port.List lists them)", what,
                     (unsigned)id);
            return true;
        }
    }
    for (size_t j = 0; j < upto; j++) {
        if (j != index && (ports[j].kind == CYPHAL_PORT_SRV) == service && ids[j] == id) {
            snprintf(why, why_len, "%s %u is %s's", what, (unsigned)id, ports[j].id_register);
            return true;
        }
    }
    return false;
}

bool cyphal_port_id_free(const cyphal_settings_t *s, size_t index, uint32_t id, size_t upto, char *why,
                         size_t why_len)
{
    const cyphal_port_kind_t kind = index < s_port_count ? s_ports[index].kind : CYPHAL_PORT_PUB;
    return !id_taken(s_ports, s->port_id, upto < s_port_count ? upto : s_port_count, index, kind, id, why, why_len);
}

uint32_t cyphal_ports_settle(cyphal_settings_t *s)
{
    uint32_t off = 0;
    char why[128];  /* not wanted: the caller says what was turned off */
    for (size_t i = 0; i < s_port_count; i++) {
        if (!cyphal_port_id_free(s, i, s->port_id[i], i, why, sizeof(why))) {
            s->port_id[i] = CYPHAL_PORT_OFF;
            off |= 1u << i;
        }
    }
    return off;
}

/* ------------------------------------------------------------------ values */

void cyphal_reg_set_string(uavcan_register_Value_1_0 *out, const char *s)
{
    uavcan_register_Value_1_0_select_string_(out);
    const size_t n = strlen(s);
    out->_string.value.count = n < sizeof(out->_string.value.elements) ? n : sizeof(out->_string.value.elements);
    memcpy(out->_string.value.elements, s, out->_string.value.count);
}

void cyphal_reg_set_natural16(uavcan_register_Value_1_0 *out, const uint16_t *values, size_t count)
{
    uavcan_register_Value_1_0_select_natural16_(out);
    out->natural16.value.count = count;
    memcpy(out->natural16.value.elements, values, count * sizeof(values[0]));
}

void cyphal_reg_set_natural32(uavcan_register_Value_1_0 *out, const uint32_t *values, size_t count)
{
    uavcan_register_Value_1_0_select_natural32_(out);
    out->natural32.value.count = count;
    memcpy(out->natural32.value.elements, values, count * sizeof(values[0]));
}

/* Nunavut packs a bit array's element i into bit i % 8 of byte i / 8 */
void cyphal_reg_set_bits(uavcan_register_Value_1_0 *out, const bool *bits, size_t count)
{
    uavcan_register_Value_1_0_select_bit_(out);
    memset(out->bit.value.bitpacked, 0, sizeof(out->bit.value.bitpacked));
    for (size_t i = 0; i < count; i++) {
        out->bit.value.bitpacked[i / 8] |= (uint8_t)(bits[i] ? 1u << (i % 8) : 0);
    }
    out->bit.value.count = count;
}

static bool bit_at(const uavcan_register_Value_1_0 *v, size_t i)
{
    return (v->bit.value.bitpacked[i / 8] >> (i % 8)) & 1u;
}

static int64_t whole_u64(uint64_t n)
{
    return n > (uint64_t)INT64_MAX ? CYPHAL_REG_NOT_WHOLE : (int64_t)n;
}

static int64_t whole_real(double d)
{
    return isfinite(d) && d == floor(d) && fabs(d) <= 9007199254740992.0    /* 2^53: exact in a double */
               ? (int64_t)d : CYPHAL_REG_NOT_WHOLE;
}

/* A numeric value (an integer, natural or real array): its element count, -1 for any other value (empty, a
 * string, bytes, bits). And element i, if it has one, as a whole number into *out: CYPHAL_REG_NOT_WHOLE
 * for a fraction, or beyond an int64 (no register's value anyway) */
static int numeric(const uavcan_register_Value_1_0 *v, size_t i, int64_t *out)
{
#define NUMERIC(kind, whole)                                    \
    if (uavcan_register_Value_1_0_is_##kind##_(v)) {            \
        if (i < v->kind.value.count) {                          \
            *out = whole(v->kind.value.elements[i]);            \
        }                                                       \
        return (int)v->kind.value.count;                        \
    }
    NUMERIC(integer64, (int64_t))
    NUMERIC(integer32, (int64_t))
    NUMERIC(integer16, (int64_t))
    NUMERIC(integer8, (int64_t))
    NUMERIC(natural64, whole_u64)
    NUMERIC(natural32, (int64_t))
    NUMERIC(natural16, (int64_t))
    NUMERIC(natural8, (int64_t))
    NUMERIC(real64, whole_real)
    NUMERIC(real32, whole_real)
    NUMERIC(real16, whole_real)
#undef NUMERIC
    return -1;
}

/* "it takes one whole number": what a write to r takes, for a refusal */
static void takes(const cyphal_register_t *r, char *why, size_t why_len)
{
    const unsigned min = r->min_count ? r->min_count : r->count;
    const char *what = r->type == CYPHAL_REG_BIT ? "bit" : "whole number";
    if (r->takes != NULL) {
        snprintf(why, why_len, "it takes %s", r->takes);
    } else if (r->type == CYPHAL_REG_STRING) {
        snprintf(why, why_len, "it takes a string");
    } else if (r->count == 1) {
        snprintf(why, why_len, "it takes one %s", what);
    } else if (min == r->count) {
        snprintf(why, why_len, "it takes %u %ss", (unsigned)r->count, what);
    } else {
        snprintf(why, why_len, "it takes %u to %u %ss", min, (unsigned)r->count, what);
    }
}

static bool count_ok(const cyphal_register_t *r, int count)
{
    return count >= (r->min_count ? r->min_count : r->count) && count <= r->count;
}

cyphal_reg_result_t cyphal_reg_take_numbers(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v,
                                            int64_t out[], char *why, size_t why_len)
{
    int64_t unused;
    const int count = numeric(v, 0, &unused);
    if (!count_ok(r, count)) {
        takes(r, why, why_len);
        return CYPHAL_REG_BAD_TYPE;
    }
    for (size_t i = 0; i < r->count; i++) {
        out[i] = CYPHAL_REG_NOT_WHOLE;
        numeric(v, i, &out[i]);
    }
    return CYPHAL_REG_OK;
}

cyphal_reg_result_t cyphal_reg_take_bits(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v, bool out[],
                                         char *why, size_t why_len)
{
    if (uavcan_register_Value_1_0_is_bit_(v)) {
        if (!count_ok(r, (int)v->bit.value.count)) {
            takes(r, why, why_len);
            return CYPHAL_REG_BAD_TYPE;
        }
        for (size_t i = 0; i < r->count; i++) {
            out[i] = i < v->bit.value.count && bit_at(v, i);
        }
        return CYPHAL_REG_OK;
    }
    int64_t n[uavcan_primitive_array_Natural64_1_0_value_ARRAY_CAPACITY_];   /* a numeric value's most */
    if (r->count > sizeof(n) / sizeof(n[0])) {
        takes(r, why, why_len);
        return CYPHAL_REG_BAD_TYPE;
    }
    const cyphal_reg_result_t res = cyphal_reg_take_numbers(r, v, n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    int64_t unused;
    const size_t given = (size_t)numeric(v, 0, &unused);
    for (size_t i = 0; i < r->count; i++) {
        if (i < given && n[i] != 0 && n[i] != 1) {
            snprintf(why, why_len, "a bit is 0 or 1");
            return CYPHAL_REG_BAD_VALUE;
        }
        out[i] = i < given && n[i] == 1;
    }
    return CYPHAL_REG_OK;
}

cyphal_reg_result_t cyphal_reg_take_string(const cyphal_register_t *r, const uavcan_register_Value_1_0 *v,
                                           const uint8_t **bytes, size_t *len, char *why, size_t why_len)
{
    if (uavcan_register_Value_1_0_is_string_(v)) {
        *bytes = v->_string.value.elements;
        *len = v->_string.value.count;
    } else if (uavcan_register_Value_1_0_is_unstructured_(v)) {
        *bytes = v->unstructured.value.elements;
        *len = v->unstructured.value.count;
    } else {
        takes(r, why, why_len);
        return CYPHAL_REG_BAD_TYPE;
    }
    return CYPHAL_REG_OK;
}

/* ------------------------------------------------------------------ the node's registers */

static void read_node_id(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    const uint16_t id = s->node_id == CYPHAL_NODE_ID_AUTO ? CYPHAL_PORT_OFF : s->node_id;     /* 65535: unset */
    cyphal_reg_set_natural16(out, &id, 1);
}

static cyphal_reg_result_t write_node_id(const cyphal_register_t *r, cyphal_settings_t *s,
                                         const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    int64_t n;
    const cyphal_reg_result_t res = cyphal_reg_take_numbers(r, v, &n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    if (!(n == CYPHAL_PORT_OFF || (n >= 0 && n <= CYPHAL_NODE_ID_MAX))) {
        snprintf(why, why_len, "a node-ID is 0 to %d, or 65535 for plug-and-play", CYPHAL_NODE_ID_MAX);
        return CYPHAL_REG_BAD_VALUE;
    }
    s->node_id = n == CYPHAL_PORT_OFF ? CYPHAL_NODE_ID_AUTO : (uint8_t)n;
    return CYPHAL_REG_OK;
}

static void read_name(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    cyphal_reg_set_string(out, s->name);
}

static cyphal_reg_result_t write_name(const cyphal_register_t *r, cyphal_settings_t *s,
                                      const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    const uint8_t *bytes;
    size_t n;
    const cyphal_reg_result_t res = cyphal_reg_take_string(r, v, &bytes, &n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    if (n > CYPHAL_NAME_MAX) {
        snprintf(why, why_len, "a name is 1 to %d characters, and this is %u", CYPHAL_NAME_MAX, (unsigned)n);
        return CYPHAL_REG_BAD_VALUE;
    }
    if (memchr(bytes, '\0', n) != NULL) {     /* which cyphal_name_valid() would take for the end */
        snprintf(why, why_len, "character 0x00 is not allowed: only lower-case letters, digits, `_` and `.`");
        return CYPHAL_REG_BAD_VALUE;
    }
    char name[CYPHAL_NAME_MAX + 1];
    memcpy(name, bytes, n);
    name[n] = '\0';
    if (!cyphal_name_valid(name, why, why_len)) {
        return CYPHAL_REG_BAD_VALUE;
    }
    memcpy(s->name, name, n + 1);
    return CYPHAL_REG_OK;
}

static void read_bitrate(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    const uint32_t b[2] = { s->bitrate, s->bitrate };
    cyphal_reg_set_natural32(out, b, 2);
}

static cyphal_reg_result_t write_bitrate(const cyphal_register_t *r, cyphal_settings_t *s,
                                         const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    int64_t n[2];
    const cyphal_reg_result_t res = cyphal_reg_take_numbers(r, v, n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    /* The second value is ignored, whatever it is */
    if (n[0] < 0 || n[0] > UINT32_MAX || !cyphal_bitrate_valid((uint32_t)n[0])) {
        snprintf(why, why_len, "the bitrate is 125000, 250000, 500000 or 1000000");
        return CYPHAL_REG_BAD_VALUE;
    }
    s->bitrate = (uint32_t)n[0];
    return CYPHAL_REG_OK;
}

static size_t port_index(const cyphal_register_t *r)
{
    return (size_t)((const cyphal_port_t *)r->ctx - s_ports);
}

static void read_port_id(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    cyphal_reg_set_natural16(out, &s->port_id[port_index(r)], 1);
}

static cyphal_reg_result_t write_port_id(const cyphal_register_t *r, cyphal_settings_t *s,
                                         const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    int64_t n;
    const cyphal_reg_result_t res = cyphal_reg_take_numbers(r, v, &n, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    const cyphal_port_kind_t kind = ((const cyphal_port_t *)r->ctx)->kind;
    if (n < 0 || n > UINT32_MAX || !cyphal_port_id_valid(kind, (uint32_t)n)) {
        snprintf(why, why_len, "a %s-ID is 0 to %d, or 65535 to turn the port off",
                 kind == CYPHAL_PORT_SRV ? "service" : "subject",
                 kind == CYPHAL_PORT_SRV ? CYPHAL_SERVICE_ID_MAX : CYPHAL_SUBJECT_ID_MAX);
        return CYPHAL_REG_BAD_VALUE;
    }
    if (!cyphal_port_id_free(s, port_index(r), (uint32_t)n, s_port_count, why, why_len)) {
        return CYPHAL_REG_BAD_VALUE;
    }
    s->port_id[port_index(r)] = (uint16_t)n;
    return CYPHAL_REG_OK;
}

static void read_port_type(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)s;
    cyphal_reg_set_string(out, ((const cyphal_port_t *)r->ctx)->type);
}

/* Immutable, with its own reason rather than "it can't be written" */
static cyphal_reg_result_t write_port_type(const cyphal_register_t *r, cyphal_settings_t *s,
                                           const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    (void)r;
    (void)s;
    (void)v;
    snprintf(why, why_len, "a port's type can't be written");
    return CYPHAL_REG_IMMUTABLE;
}

#define SETTING (CYPHAL_REG_MUTABLE | CYPHAL_REG_PERSISTENT | CYPHAL_REG_NEXT_START)

/* ------------------------------------------------------------------ the table */

/* uavcan.register.Name's rules: a-z, 0-9, `.` and `_`; not starting with a digit or `.`, not ending with
 * `.`, no empty part; at least one `.`; 255 characters at most. Not cyphal_name_valid()'s, which is a
 * node's name (GetInfo's, 50 at most, no part starting with a digit, no dot needed): the standard gives
 * the two different rules, and a register name may have a part such as `0` that a node name may not */
static bool register_name_ok(const char *name)
{
    const size_t n = name != NULL ? strlen(name) : 0;
    if (n == 0 || n > 255 || (name[0] >= '0' && name[0] <= '9') || name[0] == '.' || name[n - 1] == '.' ||
        strchr(name, '.') == NULL || strstr(name, "..") != NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool ports_ok(const cyphal_port_t *ports, size_t count, char *why, size_t why_len)
{
    if (count > CYPHAL_PORTS_MAX) {
        snprintf(why, why_len, "%u ports: the node keeps %d at most", (unsigned)count, CYPHAL_PORTS_MAX);
        return false;
    }
    static const char *const node_keys[] = { CYPHAL_NVS_KEY_NODE_ID, CYPHAL_NVS_KEY_NAME, CYPHAL_NVS_KEY_BITRATE };
    uint16_t defaults[CYPHAL_PORTS_MAX];
    char taken[96];
    for (size_t i = 0; i < count; i++) {
        const cyphal_port_t *p = &ports[i];
        if (!cyphal_port_id_valid(p->kind, p->default_id)) {
            snprintf(why, why_len, "port %s: the default %u is no %s-ID", p->name, p->default_id,
                     p->kind == CYPHAL_PORT_SRV ? "service" : "subject");
            return false;
        }
        if (!register_name_ok(p->id_register) || !register_name_ok(p->type_register) || p->type == NULL) {
            snprintf(why, why_len, "port %s: its register names are not register names", p->name);
            return false;
        }
        /* By the rule a write is held to, against the defaults before it (cyphal_port_id_free()) */
        defaults[i] = p->default_id;
        if (id_taken(ports, defaults, i, i, p->kind, p->default_id, taken, sizeof(taken))) {
            snprintf(why, why_len, "port %s: its default: %s", p->name, taken);
            return false;
        }
        const size_t key = p->nvs_key ? strlen(p->nvs_key) : 0;
        if (key == 0 || key > CYPHAL_NVS_KEY_MAX) {
            snprintf(why, why_len, "port %s: its NVS key is 1 to %d characters", p->name, CYPHAL_NVS_KEY_MAX);
            return false;
        }
        for (size_t j = 0; j < sizeof(node_keys) / sizeof(node_keys[0]); j++) {
            if (strcmp(node_keys[j], p->nvs_key) == 0) {
                snprintf(why, why_len, "port %s: its NVS key `%s` is one of the node's own", p->name, p->nvs_key);
                return false;
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(ports[j].nvs_key, p->nvs_key) == 0) {
                snprintf(why, why_len, "ports %s and %s: the same NVS key", ports[j].name, p->name);
                return false;
            }
        }
    }
    return true;
}

/* The node's own entries (build()): no name twice, each against all before it */
static bool own_names_unique(char *why, size_t why_len)
{
    for (size_t i = 1; i < s_own_count; i++) {
        for (size_t j = 0; j < i; j++) {
            if (strcmp(s_own[i].name, s_own[j].name) == 0) {
                snprintf(why, why_len, "register `%s` twice", s_own[i].name);
                return false;
            }
        }
    }
    return true;
}

/* An application's register: its size within what a value holds, and its functions and flags agreeing (never
 * CYPHAL_REG_NEXT_START, the node's own settings'; CYPHAL_REG_READ_ON_WORKER is for these alone) */
static bool app_register_ok(const cyphal_register_t *r)
{
    size_t most = 0;
    switch (r->type) {
    case CYPHAL_REG_STRING:    most = 0; break;
    case CYPHAL_REG_NATURAL16: most = uavcan_primitive_array_Natural16_1_0_value_ARRAY_CAPACITY_; break;
    case CYPHAL_REG_NATURAL32: most = uavcan_primitive_array_Natural32_1_0_value_ARRAY_CAPACITY_; break;
    case CYPHAL_REG_BIT:       most = uavcan_primitive_array_Natural64_1_0_value_ARRAY_CAPACITY_; break;
    default:                   return false;
    }
    const bool size_ok = r->type == CYPHAL_REG_STRING ? r->count == 0 && r->min_count == 0
                                                      : r->count >= 1 && r->count <= most && r->min_count <= r->count;
    return size_ok && r->read != NULL && ((r->flags & CYPHAL_REG_MUTABLE) != 0) == (r->write != NULL) &&
           !(r->flags & CYPHAL_REG_NEXT_START);
}

/* The node's own entries over these ports, and none of the application's yet */
static void build(const cyphal_port_t *ports, size_t port_count, const char *name_register)
{
    s_ports = ports;
    s_port_count = port_count;
    s_app = NULL;
    s_app_count = 0;
    s_own[0] = (cyphal_register_t){ "uavcan.node.id", CYPHAL_REG_NATURAL16, 1, 0, SETTING, NULL,
                                    read_node_id, write_node_id, NULL };
    s_own[1] = (cyphal_register_t){ name_register, CYPHAL_REG_STRING, 0, 0, SETTING, NULL, read_name, write_name, NULL };
    s_own[2] = (cyphal_register_t){ "uavcan.can.bitrate", CYPHAL_REG_NATURAL32, 2, 1, SETTING,
                                    "one or two whole numbers (the second is ignored)", read_bitrate, write_bitrate,
                                    NULL };
    s_own_count = NODE_REGISTERS;
    for (size_t i = 0; i < port_count; i++) {
        s_own[s_own_count++] = (cyphal_register_t){ ports[i].id_register, CYPHAL_REG_NATURAL16, 1, 0, SETTING, NULL,
                                                    read_port_id, write_port_id, &ports[i] };
        s_own[s_own_count++] = (cyphal_register_t){ ports[i].type_register, CYPHAL_REG_STRING, 0, 0,
                                                    CYPHAL_REG_PERSISTENT, NULL, read_port_type, write_port_type,
                                                    &ports[i] };
    }
}

bool cyphal_reg_init(const cyphal_port_t *ports, size_t port_count, const char *name_register,
                     const cyphal_register_t *app, size_t app_count, char *why, size_t why_len)
{
    bool ok = ports_ok(ports, port_count, why, why_len);
    if (ok && !register_name_ok(name_register)) {
        snprintf(why, why_len, "`%s` is not a register name", name_register ? name_register : "");
        ok = false;
    }
    if (ok) {
        build(ports, port_count, name_register);
        ok = own_names_unique(why, why_len);
    }
    if (ok) {
        /* The application's appended one at a time, so that each name is checked against all before it */
        s_app = app;
        for (size_t i = 0; ok && i < app_count; i++) {
            const cyphal_register_t *r = &app[i];
            if (!register_name_ok(r->name) || cyphal_reg_find((const uint8_t *)r->name, strlen(r->name)) >= 0) {
                snprintf(why, why_len, "register `%s`: not a register name, or taken", r->name ? r->name : "");
                ok = false;
            } else if (!app_register_ok(r)) {
                snprintf(why, why_len, "register %s: its type, size, flags or functions don't fit", r->name);
                ok = false;
            } else {
                s_app_count = i + 1;
            }
        }
    }
    if (!ok) {
        build(NULL, 0, "node.name");
    }
    return ok;
}

size_t cyphal_port_count(void)
{
    return s_port_count;
}

const cyphal_port_t *cyphal_port(size_t index)
{
    return index < s_port_count ? &s_ports[index] : NULL;
}

void cyphal_ports_default(cyphal_settings_t *s)
{
    for (size_t i = 0; i < CYPHAL_PORTS_MAX; i++) {
        s->port_id[i] = i < s_port_count ? s_ports[i].default_id : CYPHAL_PORT_OFF;
    }
}

size_t cyphal_reg_count(void)
{
    return s_own_count + s_app_count;
}

const cyphal_register_t *cyphal_reg(size_t index)
{
    return index < s_own_count ? &s_own[index] : index < cyphal_reg_count() ? &s_app[index - s_own_count] : NULL;
}

const char *cyphal_reg_name(size_t index)
{
    const cyphal_register_t *r = cyphal_reg(index);
    return r != NULL ? r->name : NULL;
}

int cyphal_reg_find(const uint8_t *name, size_t len)
{
    for (size_t i = 0; i < cyphal_reg_count(); i++) {
        const char *n = cyphal_reg_name(i);
        if (strlen(n) == len && memcmp(n, name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool cyphal_reg_mutable(size_t index)
{
    const cyphal_register_t *r = cyphal_reg(index);
    return r != NULL && (r->flags & CYPHAL_REG_MUTABLE);
}

const char *cyphal_reg_type(size_t index, char *buf, size_t len)
{
    static const char *const names[] = { "string", "natural16", "natural32", "bit" };
    const cyphal_register_t *r = cyphal_reg(index);
    if (r == NULL) {
        if (len > 0) {
            buf[0] = '\0';
        }
    } else if (r->type == CYPHAL_REG_STRING) {
        snprintf(buf, len, "string");
    } else {
        snprintf(buf, len, "%s[%u]", names[r->type], (unsigned)r->count);
    }
    return buf;
}

void cyphal_reg_read(const cyphal_settings_t *s, size_t index, uavcan_register_Value_1_0 *out)
{
    memset(out, 0, sizeof(*out));
    const cyphal_register_t *r = cyphal_reg(index);
    if (r == NULL) {
        uavcan_register_Value_1_0_select_empty_(out);
        return;
    }
    r->read(r, s, out);
}

cyphal_reg_result_t cyphal_reg_write(cyphal_settings_t *s, size_t index, const uavcan_register_Value_1_0 *v,
                                     char *why, size_t why_len)
{
    const cyphal_register_t *r = cyphal_reg(index);
    if (r == NULL) {
        snprintf(why, why_len, "no register there");
        return CYPHAL_REG_IMMUTABLE;
    }
    if (r->write == NULL) {
        snprintf(why, why_len, "it can't be written");
        return CYPHAL_REG_IMMUTABLE;
    }
    return r->write(r, s, v, why, why_len);
}

/* ------------------------------------------------------------------ the console's words */

/* Decimal digits only, at most 19 of them (an int64's reach) */
static bool parse_number(const char *s, uint64_t *out)
{
    const size_t n = strlen(s);
    if (n == 0 || n > 19 || strspn(s, "0123456789") != n) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

cyphal_reg_result_t cyphal_reg_parse(size_t index, int argc, char *const argv[], uavcan_register_Value_1_0 *out,
                                     char *why, size_t why_len)
{
    memset(out, 0, sizeof(*out));
    const cyphal_register_t *r = cyphal_reg(index);
    if (r == NULL) {
        snprintf(why, why_len, "no register there");
        return CYPHAL_REG_IMMUTABLE;
    }
    const size_t words = argc > 0 ? (size_t)argc : 0;
    switch (r->type) {
    case CYPHAL_REG_STRING:
        if (words != 1) {
            snprintf(why, why_len, "it takes a string, one word");
            return CYPHAL_REG_BAD_TYPE;
        }
        cyphal_reg_set_string(out, argv[0]);
        return CYPHAL_REG_OK;
    case CYPHAL_REG_BIT:
        if (words > uavcan_primitive_array_Bit_1_0_value_ARRAY_CAPACITY_) {
            takes(r, why, why_len);
            return CYPHAL_REG_BAD_TYPE;
        }
        uavcan_register_Value_1_0_select_bit_(out);
        for (size_t i = 0; i < words; i++) {
            if (strcmp(argv[i], "0") != 0 && strcmp(argv[i], "1") != 0) {
                snprintf(why, why_len, "`%s` is not 0 or 1", argv[i]);
                return CYPHAL_REG_BAD_TYPE;
            }
            out->bit.value.bitpacked[i / 8] |= (uint8_t)(argv[i][0] == '1' ? 1u << (i % 8) : 0);
        }
        out->bit.value.count = words;
        return CYPHAL_REG_OK;
    default:
        /* As natural64, the widest: a number too big for the register is the write's to refuse, with its range */
        if (words > uavcan_primitive_array_Natural64_1_0_value_ARRAY_CAPACITY_) {
            takes(r, why, why_len);
            return CYPHAL_REG_BAD_TYPE;
        }
        uavcan_register_Value_1_0_select_natural64_(out);
        for (size_t i = 0; i < words; i++) {
            if (!parse_number(argv[i], &out->natural64.value.elements[i])) {
                snprintf(why, why_len, "`%s` is not a whole number from 0 up", argv[i]);
                return CYPHAL_REG_BAD_TYPE;
            }
        }
        out->natural64.value.count = words;
        return CYPHAL_REG_OK;
    }
}

void cyphal_reg_format(const uavcan_register_Value_1_0 *v, char *out, size_t len)
{
    if (len == 0) {
        return;
    }
    out[0] = '\0';
    if (uavcan_register_Value_1_0_is_string_(v)) {
        const size_t n = v->_string.value.count < len - 1 ? v->_string.value.count : len - 1;
        memcpy(out, v->_string.value.elements, n);
        out[n] = '\0';
        return;
    }
    const bool bits = uavcan_register_Value_1_0_is_bit_(v);
    int64_t unused;
    const int count = bits ? (int)v->bit.value.count : numeric(v, 0, &unused);
    if (count < 0) {
        snprintf(out, len, "(empty)");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < count && used < len; i++) {
        int64_t n = bits ? bit_at(v, (size_t)i) : CYPHAL_REG_NOT_WHOLE;
        if (!bits) {
            numeric(v, (size_t)i, &n);
        }
        const int w = n != CYPHAL_REG_NOT_WHOLE ? snprintf(out + used, len - used, "%s%lld", i ? " " : "", (long long)n)
                                                : snprintf(out + used, len - used, "%s?", i ? " " : "");
        used += w > 0 ? (size_t)w : 0;
    }
}
