/*
 * `cyphal`: the node's status and settings on the console (the kit's README, "cyphal").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canbus.h"
#include "cyphal.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if CONFIG_CYPHAL_PNP_ALLOCATOR
#include "cyphal_alloc.h"
#define ALLOC_HINT " | allocator [on|off] | allocations [forget <id>|clear]"
#define ALLOC_HELP " `allocator` shows the plug-and-play allocator; `on` or `off` sets it, from the node's next start " \
                   "(it allocates only with a fixed node-ID). `allocations` lists the node-IDs it has given; " \
                   "`forget <id>` frees one, `clear` all."
#else
#define ALLOC_HINT ""
#define ALLOC_HELP ""
#endif

static const char *s_how_to_start;     /* the application's words for what turns the node on */

static void print_node_id(uint8_t id)
{
    if (id == CYPHAL_NODE_ID_AUTO) {
        printf("plug-and-play");
    } else {
        printf("node-ID %u (fixed)", id);
    }
}

static void print_settings(const char *lead, const cyphal_settings_t *s)
{
    char rate[24];
    printf("%s", lead);
    print_node_id(s->node_id);
    printf(", %s, %s\n", s->name, canbus_format_bitrate(rate, sizeof(rate), s->bitrate));
}

/* The port registers in `now` that differ from `was`, on a line of their own: nothing if none do */
static void print_ports_changed(const cyphal_settings_t *now, const cyphal_settings_t *was)
{
    const char *sep = "    and ";
    for (size_t i = 0; i < cyphal_port_count(); i++) {
        if (now->port_id[i] != was->port_id[i]) {
            printf("%s%s %u", sep, cyphal_port(i)->id_register, now->port_id[i]);
            sep = ", ";
        }
    }
    if (sep[0] == ',') {
        printf("\n");
    }
}

/* One register, as `cyphal register` lists it: the saved value, and for a node setting the one in use when
 * that differs */
static void print_register(size_t index, const cyphal_status_t *st)
{
    static uavcan_register_Value_1_0 v;     /* 0.5 KB: off the console's stack */
    const cyphal_register_t *r = cyphal_reg(index);
    char saved[64], running[64], type[24];
    cyphal_reg_read(&st->saved, index, &v);
    cyphal_reg_format(&v, saved, sizeof(saved));
    printf("%-31s %-22s %s, %s, %s", r->name, saved, cyphal_reg_type(index, type, sizeof(type)),
           r->flags & CYPHAL_REG_MUTABLE ? "mutable" : "immutable",
           r->flags & CYPHAL_REG_PERSISTENT ? "persistent" : "not persistent");
    if ((r->flags & CYPHAL_REG_MUTABLE) && (r->flags & CYPHAL_REG_NEXT_START) && st->state != CYPHAL_OFF &&
        st->state != CYPHAL_FAILED) {
        cyphal_reg_read(&st->running, index, &v);
        cyphal_reg_format(&v, running, sizeof(running));
        if (strcmp(saved, running) != 0) {
            printf("; %s until the node's next start", running);
        }
    }
    printf("\n");
}

#if CONFIG_CYPHAL_PNP_ALLOCATOR
/* The plug-and-play allocator's lines: in `cyphal` only when it is on (saved or running) or its table has rows, so that
 * a node that has never allocated reports as it always did; with `always` (`cyphal allocator`), every time */
static void print_allocator(bool always)
{
    cyphal_allocator_status_t a;
    cyphal_allocator_status(&a);
    if (!always && !a.saved && !a.running && a.rows == 0) {
        return;
    }
    printf("  allocator: %s, node-IDs %u to %u", a.running ? "on" : "off", a.first, a.last);
    switch (a.state) {
    case CYPHAL_ALLOCATOR_NEEDS_FIXED:
        printf("; allocating nothing: the node-ID is plug-and-play, and an allocator needs a fixed one");
        break;
    case CYPHAL_ALLOCATOR_LISTENING:
        printf("; quiet while the node listens");
        break;
    case CYPHAL_ALLOCATOR_ACTIVE:
        printf("; answering requests");
        break;
    case CYPHAL_ALLOCATOR_YIELDED:
        printf("; stopped: node %u answers requests too (one allocator a bus), until the node's next start", a.other);
        break;
    case CYPHAL_ALLOCATOR_STOPPED:
        printf("; stopped with the node");
        break;
    case CYPHAL_ALLOCATOR_RESET:
        printf("; stopped by a reset, until the node's next start");
        break;
    default:
        break;
    }
    printf("\n    table %u of %u rows (%u heard on the bus)%s; requests %lu, allocations %lu, repeats %lu, "
           "refused for no room %lu, yielded %lu, own node-ID %lu, shed %lu\n", (unsigned)a.rows, (unsigned)a.capacity,
           (unsigned)a.heard_rows, a.unsaved ? ", a row not saved yet" : "", (unsigned long)a.requests,
           (unsigned long)a.allocations, (unsigned long)a.repeats, (unsigned long)a.refused, (unsigned long)a.yielded,
           (unsigned long)a.own, (unsigned long)a.shed);
    if (a.saved != a.running) {
        printf("    %s from the node's next start (`cyphal restart`)\n", a.saved ? "on" : "off");
    }
}
#endif

static void print_status(void)
{
    cyphal_status_t st;
    cyphal_status(&st);
    switch (st.state) {
    case CYPHAL_OFF:
        printf("cyphal: off (%s)\n", s_how_to_start);
        print_settings("  settings: ", &st.saved);
        break;
    case CYPHAL_FAILED:
        if (st.stop_failed) {
            printf("cyphal: not started on GPIO%d and GPIO%d: the controller would not stop (%s)\n", st.tx_gpio,
                   st.rx_gpio, esp_err_to_name(st.error));
        } else {
            printf("cyphal: not started on GPIO%d and GPIO%d: %s%s\n", st.tx_gpio, st.rx_gpio,
                   st.error == ESP_ERR_INVALID_STATE ? "the controller is in use by " : esp_err_to_name(st.error),
                   st.error == ESP_ERR_INVALID_STATE && canbus_owner() ? canbus_owner() : "");
        }
        print_settings("  settings: ", &st.saved);
        break;
    default:
        if (st.state == CYPHAL_WAITING) {
            printf("cyphal: waiting for a node-ID");
        } else if (st.state == CYPHAL_LISTENING) {
            printf("cyphal: listening, before going online as node %u (fixed)", st.node_id);
        } else if (st.state == CYPHAL_IN_USE) {
            printf("cyphal: stopped: node-ID %u is in use by another node", st.node_id);
        } else {
            printf("cyphal: node %u (%s)", st.node_id, st.allocated ? "allocated" : "fixed");
        }
        char rate[24];
        printf(", %s, %s on GPIO%d (TX), GPIO%d (RX)\n", st.running.name,
               canbus_format_bitrate(rate, sizeof(rate), st.running.bitrate), st.tx_gpio, st.rx_gpio);
        break;
    }
    printf("  unique id ");
    for (int i = 0; i < CYPHAL_UNIQUE_ID_SIZE; i++) {
        printf("%02x", st.unique_id[i]);
    }
    /* In hex and in decimal: allocators show it either way (cyphal_service's pnp-list in decimal) */
    printf(", pseudo-ID %012llx (%llu)\n", (unsigned long long)st.pseudo_id, (unsigned long long)st.pseudo_id);
    if (st.state == CYPHAL_OFF || st.state == CYPHAL_FAILED) {
        return;
    }
    canbus_status_t bus;
    canbus_status(&bus);
    printf("  bus: %s, tx errors %u, rx errors %u, bus-off %lu times", canbus_state_name(bus.state), bus.tx_errors,
           bus.rx_errors, (unsigned long)bus.bus_off);
    if (bus.state == CANBUS_BUS_OFF && st.recover_in_s > 0) {
        printf(" (back to back: recovering in %lu s)", (unsigned long)st.recover_in_s);
    }
    printf("\n");
    printf("  transfers: sent %lu, received %lu; frames: sent %lu, received %lu; dropped %lu\n",
           (unsigned long)st.transfers_sent, (unsigned long)st.transfers_received, (unsigned long)bus.tx_frames,
           (unsigned long)bus.rx_frames, (unsigned long)st.transfers_dropped);
    printf("  up %lu s; frames given to the controller %lu, not sent %lu, waiting %lu; not read (queue full) %lu; "
           "send queue at most %lu of %lu frames\n", (unsigned long)st.uptime_s, (unsigned long)st.frames_queued,
           (unsigned long)bus.tx_failed, (unsigned long)bus.tx_pending, (unsigned long)bus.rx_dropped,
           (unsigned long)st.txq_peak, (unsigned long)st.txq_capacity);
    printf("  memory: libcanard heap %lu of %lu bytes (%lu at most%s), stack %lu bytes never used; "
           "internal heap %u free, %u lowest\n", (unsigned long)st.heap_used, (unsigned long)st.heap_size,
           (unsigned long)st.heap_peak, st.heap_failed ? ", and it ran out" : "", (unsigned long)st.stack_unused,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    printf("  services: registers read %lu, written %lu, refused %lu, listed %lu; commands %lu, bad %lu; busy %lu; "
           "port lists %lu\n", (unsigned long)st.services.register_reads, (unsigned long)st.services.register_writes,
           (unsigned long)st.services.register_refused, (unsigned long)st.services.register_lists,
           (unsigned long)st.services.commands, (unsigned long)st.services.commands_refused,
           (unsigned long)st.services.busy, (unsigned long)st.services.port_lists);
    static const char *const HEALTH[] = { "nominal", "advisory", "caution", "warning" };
    printf("  application: requests %lu, messages %lu; health %s\n", (unsigned long)st.services.app_requests,
           (unsigned long)st.services.app_messages, HEALTH[st.health & 3]);
    /* Each published port: how many went, and how many a newer one replaced (depth 1) or a full queue refused */
    for (size_t i = 0; i < st.mail_count; i++) {
        const cyphal_mail_t *m = &st.mail[i];
        const cyphal_port_t *p = cyphal_port(m->port);
        printf("%s%s %lu (%lu %s)%s", i == 0 ? "  published: " : "", p != NULL ? p->name : "?",
               (unsigned long)m->published, (unsigned long)(m->depth > 1 ? m->refused : m->replaced),
               m->depth > 1 ? "refused, full" : "replaced", i + 1 < st.mail_count ? ", " : "\n");
    }
    printf("  worker: %lu of %lu job slots held; its stack %lu of %d bytes never used", (unsigned long)st.jobs_held,
           (unsigned long)st.job_slots, (unsigned long)st.worker_stack_unused, CONFIG_CYPHAL_WORKER_STACK);
    if (st.slow_worker) {
        printf("; the second worker %lu of %d, its stack %lu of %d never used", (unsigned long)st.slow_held,
               CYPHAL_SLOW_JOBS, (unsigned long)st.slow_stack_unused, CONFIG_CYPHAL_SLOW_STACK);
    }
    printf("\n");
    if (!cyphal_settings_equal(&st.saved, &st.running)) {
        print_settings("  from the next start (`cyphal restart`): ", &st.saved);
        print_ports_changed(&st.saved, &st.running);
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    print_allocator(false);
#endif
}

static int saved(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        printf("cyphal: not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("cyphal: %s, from the node's next start (`cyphal restart`, or a restart of the board)\n", what);
    return 0;
}

/* `cyphal register [<name> [<value>...]]`: the same table, rules and saved settings as Cyphal's
 * uavcan.register.Access and List */
static int cmd_register(int argc, char **argv)
{
    static cyphal_status_t st;
    if (argc == 2) {
        cyphal_status(&st);
        for (size_t i = 0; i < cyphal_reg_count(); i++) {
            print_register(i, &st);
        }
        return 0;
    }
    const int index = cyphal_reg_find((const uint8_t *)argv[2], strlen(argv[2]));
    if (index < 0) {
        printf("cyphal: no register `%s`: `cyphal register` lists them\n", argv[2]);
        return 1;
    }
    if (argc == 3) {
        cyphal_status(&st);
        print_register((size_t)index, &st);
        return 0;
    }
    static uavcan_register_Value_1_0 v;     /* 0.5 KB: off the console's stack */
    char why[128], text[64], what[112];
    /* The words, then the write, which leaves the register's value as it is now in v */
    const esp_err_t err = cyphal_reg_parse((size_t)index, argc - 3, &argv[3], &v, why, sizeof(why)) == CYPHAL_REG_OK
                              ? cyphal_write_register((size_t)index, &v, &v, why, sizeof(why))
                              : ESP_ERR_INVALID_ARG;
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED) {
        printf("cyphal: %s not written: %s\n", argv[2], why);
        return 1;
    }
    cyphal_reg_format(&v, text, sizeof(text));
    snprintf(what, sizeof(what), "%s = %s", argv[2], text);
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    /* The application's: in effect already. The allocator's register is kept as one of them but applies at the node's
     * next start, as the node's settings do */
    if (!(cyphal_reg((size_t)index)->flags & CYPHAL_REG_NEXT_START) &&
        strcmp(cyphal_reg((size_t)index)->name, CONFIG_CYPHAL_ALLOCATOR_REGISTER) != 0) {
#else
    if (!(cyphal_reg((size_t)index)->flags & CYPHAL_REG_NEXT_START)) {      /* the application's: in effect already */
#endif
        printf("cyphal: %s%s%s\n", what, err == ESP_OK ? "" : ": ", err == ESP_OK ? "" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    return saved(err, what);
}

/* 0-127, digits only */
static bool parse_node_id(const char *s, uint8_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || s[0] < '0' || s[0] > '9' || v > CYPHAL_NODE_ID_MAX) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

#if CONFIG_CYPHAL_PNP_ALLOCATOR
/* `cyphal allocator [on|off]` */
static int cmd_allocator(int argc, char **argv)
{
    if (argc == 2) {
        print_allocator(true);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        const bool on = strcmp(argv[2], "on") == 0;
        return saved(cyphal_set_allocator(on), on ? "the allocator on (it allocates with a fixed node-ID only)"
                                                  : "the allocator off");
    }
    printf("usage: cyphal allocator [on|off]\n");
    return 1;
}

/* `cyphal allocations [forget <id> | clear]` */
static int cmd_allocations(int argc, char **argv)
{
    if (argc == 2) {
        /* One snapshot of the table, so that the listing is one moment's; PSRAM where there is some */
        cyphal_alloc_row_t *rows = calloc(CONFIG_CYPHAL_ALLOCATOR_TABLE, sizeof(*rows));
        if (rows == NULL) {
            printf("cyphal: no memory for the listing\n");
            return 1;
        }
        const size_t n = cyphal_allocator_rows(rows, CONFIG_CYPHAL_ALLOCATOR_TABLE);
        printf("cyphal: allocations, %u of %d rows\n", (unsigned)n, CONFIG_CYPHAL_ALLOCATOR_TABLE);
        for (size_t i = 0; i < n; i++) {
            if (rows[i].flags & CYPHAL_ALLOC_ROW_HEARD) {
                printf("  node %3u  heard on the bus, never given\n", rows[i].node_id);
            } else {
                printf("  node %3u  pseudo-ID %012llx\n", rows[i].node_id, (unsigned long long)rows[i].pseudo_id);
            }
        }
        free(rows);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        const esp_err_t err = cyphal_allocator_clear();
        printf("cyphal: %s\n", err == ESP_OK ? "every row forgotten, saved" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    uint8_t id = 0;
    if (argc != 4 || strcmp(argv[2], "forget") != 0) {
        printf("usage: cyphal allocations [forget <id>|clear]\n");
        return 1;
    }
    if (!parse_node_id(argv[3], &id)) {
        printf("cyphal: a node-ID is 0 to 127\n");
        return 1;
    }
    const esp_err_t err = cyphal_allocator_forget(id);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("cyphal: no row has node %u\n", id);
    } else {
        printf("cyphal: node %u forgotten%s%s\n", id, err == ESP_OK ? ", saved" : "; not saved: ",
               err == ESP_OK ? "" : esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}
#endif

static int cmd_cyphal(int argc, char **argv)
{
    if (argc == 1) {
        print_status();
        return 0;
    }
    const char *sub = argv[1];
    char what[96];
    if (strcmp(sub, "id") == 0 && argc == 3) {
        uint8_t id = CYPHAL_NODE_ID_AUTO;
        if (strcmp(argv[2], "auto") != 0 && !parse_node_id(argv[2], &id)) {
            printf("cyphal: a node-ID is 0 to 127, or `auto` for plug-and-play\n");
            return 1;
        }
        if (id == CYPHAL_NODE_ID_AUTO) {
            snprintf(what, sizeof(what), "plug-and-play");
        } else {
            snprintf(what, sizeof(what), "node-ID %u, fixed", id);
        }
        return saved(cyphal_set_node_id(id), what);
    }
    if (strcmp(sub, "name") == 0 && argc == 3) {
        const bool reset = strcmp(argv[2], "--reset") == 0;
        const char *name = reset ? CONFIG_CYPHAL_DEFAULT_NAME : argv[2];
        char why[128];
        const esp_err_t err = cyphal_set_name(name, why, sizeof(why));
        if (err == ESP_ERR_INVALID_ARG) {
            printf("cyphal: not a name: %s\n", why);
            return 1;
        }
        snprintf(what, sizeof(what), "the name %s%s", reset ? "back to " : "", name);
        return saved(err, what);
    }
    if (strcmp(sub, "bitrate") == 0 && argc == 3) {
        uint32_t bitrate = 0;
        const esp_err_t err = canbus_parse_bitrate(argv[2], &bitrate) ? cyphal_set_bitrate(bitrate) : ESP_ERR_INVALID_ARG;
        if (err == ESP_ERR_INVALID_ARG) {
            printf("cyphal: the bitrate is 125k, 250k, 500k or 1m\n");
            return 1;
        }
        return saved(err, canbus_format_bitrate(what, sizeof(what), bitrate));
    }
    if (strcmp(sub, "restart") == 0 && argc == 2) {
        const esp_err_t err = cyphal_restart();
        if (err == ESP_ERR_INVALID_STATE) {
            cyphal_status_t st;
            cyphal_status(&st);
            if (st.state == CYPHAL_FAILED) {
                printf("cyphal: it didn't start, so it has no pins to start again on: restart the board\n");
            } else {
                printf("cyphal: off, so there is nothing to restart (%s)\n", s_how_to_start);
            }
            return 1;
        }
        print_status();
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "reset") == 0 && argc == 2) {
        return saved(cyphal_reset_settings(), "plug-and-play, the default name, 1 Mbit/s and every port's default ID");
    }
    if (strcmp(sub, "register") == 0) {
        return cmd_register(argc, argv);
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    if (strcmp(sub, "allocator") == 0) {
        return cmd_allocator(argc, argv);
    }
    if (strcmp(sub, "allocations") == 0) {
        return cmd_allocations(argc, argv);
    }
#endif
    printf("usage: cyphal [id <0-127>|auto | name <name>|--reset | bitrate <125k|250k|500k|1m> | restart | reset | "
           "register [<name> [<value>...]]" ALLOC_HINT "]\n");
    return 1;
}

void cyphal_register_commands(const char *how_to_start)
{
    s_how_to_start = how_to_start ? how_to_start : "not started by this firmware";
    const esp_console_cmd_t cmd = {
        .command = "cyphal",
        .help = "The Cyphal node on the CAN bus: alone, its state, node-ID, name, unique ID and plug-and-play pseudo-ID, "
                "the bus and the counters. "
                "`id` sets a fixed node-ID (or `auto`: plug-and-play), `name` the name GetInfo reports, `bitrate` the "
                "bus's; each from the node's next start. `restart` starts it again with the settings as they are; "
                "`reset` puts the settings back to the defaults. `register` lists the node's Cyphal registers, which are "
                "the same settings, the ports' IDs and the application's own; with a name, reads one; with a value "
                "too, writes it, saved: the node's settings and the ports' IDs from the node's next start, the "
                "application's (the holo's calibration, drive policy and touch reporting) at once." ALLOC_HELP,
        .hint = "[id <0-127>|auto | name <name>|--reset | bitrate <125k|250k|500k|1m> | restart | reset | "
                "register [<name> [<value>...]]" ALLOC_HINT "]",
        .func = cmd_cyphal,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
