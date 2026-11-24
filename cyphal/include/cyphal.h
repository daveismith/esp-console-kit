/*
 * cyphal -- a Cyphal/CAN node on the board's CAN controller (canbus.h), on libcanard: it takes a
 * node-ID (fixed, or by plug-and-play), sends uavcan.node.Heartbeat once a second and
 * uavcan.node.port.List every 10 s, answers uavcan.node.GetInfo, uavcan.register.Access and List,
 * and uavcan.node.ExecuteCommand, and stops if another node turns out to have its node-ID
 * (the kit's README, "cyphal").
 *
 * The application decides when it runs and on which pins (holo-player-fw hands it its host link's
 * pins A and B in `link mode can`; the dome panel starts it at boot on its CAN Pal's pins).
 * Nothing here knows about the board beyond canbus, so that the node serves any firmware: who the
 * board is (the default name, the name register's name, the unique ID's prefix, the hardware
 * version) and where the tasks run are Kconfig options
 * (Kconfig), which the application pins in its sdkconfig.defaults; its own ports and what a
 * restart, a factory reset and identify do are cyphal_app_t, given to cyphal_init().
 *
 * Its settings (the node-ID, fixed or plug-and-play; the name; the bitrate; each application
 * port's ID) are registers (cyphal_registers.h), kept in NVS, namespace "cyphal", and apply from
 * the node's next start. None is a credential or a counter, so all are in a configuration backup.
 * The application may add registers of its own, kept and applied by itself (cyphal_app_t).
 *
 * Plug-and-play is uavcan.pnp.NodeIDAllocationData.1.0, as an allocatee that assumes nothing about
 * who answers: the node-ID it is given is not saved, and is asked for again at every start.
 *
 * The application's ports are served, published and subscribed to through handlers it gives
 * (cyphal_handler_t): a request is decoded on the node's task into a job slot and worked on, and its
 * response serialized, on the node's worker task (or, for one whose work can take long, its second
 * worker); a message received is handed to the application on the node's task, for a mailbox of its own; a
 * message the application publishes goes into the port's mailbox (cyphal_publish()), from any task, and the
 * node's task gives it its port, priority and transfer-ID. A mailbox of depth 1 keeps the newest, replacing
 * one not yet sent; a deeper one is a queue that never replaces. libcanard stays the node task's alone.
 *
 * With CONFIG_CYPHAL_CLIENT the node also calls services on other nodes (cyphal_client.h): the application's
 * CYPHAL_PORT_CLI ports and the standard services on their fixed IDs, the response handed to a callback on the
 * node's worker.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "cyphal_registers.h"
#include "cyphal_util.h"
#if CONFIG_CYPHAL_CLIENT
#include "cyphal_client.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CYPHAL_LISTEN_MS     2000    /* how long a fixed node-ID listens for itself before going online */
#define CYPHAL_APP_REQUEST_MAX  320  /* an application's request, deserialized, in a job slot: bytes at most */
#define CYPHAL_PUBLISH_MAX      400  /* an application's message, serialized, in its mailbox: bytes at most */
#define CYPHAL_PUBLISH_DEPTH_MAX  8  /* messages a published port's mailbox holds at most */
#define CYPHAL_SLOW_JOBS        2    /* requests in the second worker's hands at once (cyphal_handler_t.slow) */
#define CYPHAL_FIXED_SERVICES_MAX 4  /* the application's services on IDs the standard fixes (cyphal_app_t) */

/* uavcan.node.Health, for cyphal_set_health() */
#define CYPHAL_HEALTH_NOMINAL   0
#define CYPHAL_HEALTH_ADVISORY  1
#define CYPHAL_HEALTH_CAUTION   2
#define CYPHAL_HEALTH_WARNING   3

typedef enum {
    CYPHAL_OFF,         /* not started: nobody gave it pins */
    CYPHAL_FAILED,      /* the controller would not start, or (a restart) stop: cyphal_status_t.error says why */
    CYPHAL_WAITING,     /* plug-and-play: anonymous, asking for a node-ID until an allocator answers; then
                         * holding it until CYPHAL_PNP_LISTEN_US after the start, still listening */
    CYPHAL_LISTENING,   /* a fixed node-ID, listening for another node with it before going online */
    CYPHAL_ONLINE,      /* heartbeat and GetInfo on node_id */
    CYPHAL_IN_USE,      /* stopped: another node has its node-ID. Until the next start */
} cyphal_state_t;

/* cyphal_settings_t (the node-ID setting, the name, the bitrate) is in cyphal_util.h */

/* The services' counters, since the node started: register reads (Access with no value, or for no
 * register), writes done and refused, List calls, ExecuteCommand calls and the bad commands among them,
 * requests answered busy, or left unanswered for want of a job slot (a read of a CYPHAL_REG_READ_ON_WORKER
 * register, an application's request whose busy() gives none), port lists published */
typedef struct {
    uint32_t register_reads, register_writes, register_refused, register_lists, commands, commands_refused;
    uint32_t busy, port_lists;
    /* The application's ports: requests handed to the worker, messages received on its subscriptions */
    uint32_t app_requests, app_messages;
} cyphal_services_t;

/* A published port's mailbox, since the start: messages queued for the bus; with depth 1, those a newer one
 * replaced before they went; with a deeper one, those refused because it was full (cyphal_publish()) */
typedef struct {
    size_t port;                        /* its index in cyphal_app_t.ports */
    uint8_t depth;
    uint32_t published, replaced, refused;
} cyphal_mail_t;

typedef struct {
    cyphal_state_t state;
    esp_err_t error;                    /* CYPHAL_FAILED: what canbus_start(), or canbus_stop(), said */
    bool stop_failed;                   /* CYPHAL_FAILED: a restart's stop failed, and the controller still runs */
    uint8_t node_id;                    /* the one it runs (or listens, or stopped) on; CYPHAL_NODE_ID_AUTO while waiting */
    bool allocated;                     /* node_id came from plug-and-play */
    cyphal_settings_t running;          /* as the node started; zeros before the first start */
    cyphal_settings_t saved;            /* from the next start */
    int tx_gpio, rx_gpio;               /* -1 before the first start */
    uint8_t unique_id[CYPHAL_UNIQUE_ID_SIZE];
    uint64_t pseudo_id;                 /* what plug-and-play asks with (cyphal_pseudo_id()) */
    uint32_t uptime_s;                  /* since the node started */
    /* Since the node started: */
    uint32_t transfers_sent;            /* queued for the bus: heartbeats, GetInfo responses, allocation requests */
    uint32_t transfers_received;        /* reassembled, CRC checked: GetInfo requests, allocation messages */
    uint32_t transfers_dropped;         /* given up on: not on the bus by their deadline, or no room to queue */
    uint32_t frames_queued;             /* handed to the controller */
    uint32_t recover_in_s;              /* bus-off, backing off: seconds until the node asks for the recovery; 0 otherwise */
    /* libcanard's heap (CONFIG_CYPHAL_HEAP_SIZE), and the node task's stack */
    uint32_t heap_used, heap_peak, heap_size, heap_failed;
    uint32_t stack_unused;              /* bytes of the task's stack never used, 0 when no task */
    cyphal_services_t services;
    uint32_t jobs_held, job_slots;      /* job slots not free (with the worker, or done and not yet sent), of all */
    uint32_t worker_stack_unused;       /* bytes of the worker's stack never used, 0 before the first start */
    bool slow_worker;                   /* the second worker runs (a handler or identify is slow: cyphal_handler_t,
                                         * cyphal_app_t) */
    uint32_t slow_held;                 /* job slots in its hands now, of CYPHAL_SLOW_JOBS */
    uint32_t slow_stack_unused;         /* bytes of its stack never used, 0 before the first start */
    uint32_t txq_peak, txq_capacity;    /* libcanard's transmit queue: the most frames in it since the start, of all */
    uint8_t health;                     /* the heartbeat's: CYPHAL_HEALTH_* (cyphal_set_health()) */
    cyphal_mail_t mail[CYPHAL_PORTS_MAX];   /* the published ports' mailboxes, in the handlers' order */
    size_t mail_count;
} cyphal_status_t;

/*
 * What the node does with one of the application's ports (cyphal_app_t.handlers). A port without a handler
 * is neither served, published nor subscribed to, and isn't in uavcan.node.port.List, whatever its register
 * holds. Each function here must not keep the pointers it is given.
 */
typedef struct {
    size_t port;                /* its index in cyphal_app_t.ports; its kind is the port's */
    /* CYPHAL_PORT_SRV and CYPHAL_PORT_SUB: the request's or the message's extent, which libcanard cuts a
     * transfer to. CYPHAL_PORT_CLI (CONFIG_CYPHAL_CLIENT): the response's, CYPHAL_CALL_RESPONSE_MAX at most, and
     * nothing else of the handler's; the port is then called with cyphal_call() (cyphal_client.h) */
    size_t extent;
    /* CYPHAL_PORT_SRV. decode(), on the node's task: the request's payload deserialized into `in`, in_size
     * bytes (CYPHAL_APP_REQUEST_MAX at most) of a job slot, zeroed first, off any stack; false for a request
     * that doesn't deserialize, which then has no answer. work(), on the worker: the response to `in` from
     * node `from`, serialized into `out` (room bytes); its size, 0 for none. busy(), on the node's task, when
     * the worker already holds every job slot: the response that says so, serialized; its size, or 0 for no
     * response at all, for a type that has no way to say busy (uavcan.file.List: its empty name is the end of
     * the list), so that the client times out and asks again. Counted as busy either way */
    bool (*decode)(const uint8_t *payload, size_t size, void *in);
    size_t in_size;
    size_t (*work)(const void *in, uint8_t from, uint8_t *out, size_t room);
    size_t (*busy)(uint8_t *out, size_t room);
    /* CYPHAL_PORT_SRV: work() can take long (a file opened, an image decoded, a scene applied). It runs on the
     * node's second worker, a task of its own (CONFIG_CYPHAL_SLOW_STACK), in the order handed over, so that the
     * worker's own jobs and the application's poll never wait behind it. CYPHAL_SLOW_JOBS of the job slots at
     * most are its at once: another such request is answered busy() */
    bool slow;
    /* CYPHAL_PORT_SUB, on the node's task, which it must not hold up: a message from node `from` (255:
     * anonymous). A copy into the application's own mailbox, and cyphal_wake_worker() for the work */
    void (*on_message)(const uint8_t *payload, size_t size, uint8_t from);
    /* CYPHAL_PORT_PUB: the largest message cyphal_publish() will be given for it, serialized
     * (CYPHAL_PUBLISH_MAX at most): its mailbox's buffers' size. And how many messages may wait in it
     * (CYPHAL_PUBLISH_DEPTH_MAX at most): 0 or 1 for the newest only, a state's, where a message replaces one not
     * yet sent; more for a queue of happenings, none of which may be lost, which refuses one when full */
    size_t publish_max;
    uint8_t publish_depth;
    /* cyphal_app_t.fixed_services only: the service-ID the standard fixes for it (uavcan.file.List's 406). `port`
     * is then ignored */
    uint16_t fixed_id;
} cyphal_handler_t;

/*
 * What the application gives the node: its own ports, whose IDs are registers; registers of its own;
 * and the means for uavcan.node.ExecuteCommand. Each function here runs on the node's worker task,
 * which may wait (a register's read excepted: cyphal_register_t).
 */
typedef struct {
    const cyphal_port_t *ports;         /* kept, not copied: a static table. NULL for none */
    size_t port_count;
    /* Its registers, after the node's own (cyphal_register_t's rules: kept, applied and saved by the
     * application, never CYPHAL_REG_NEXT_START). Kept, not copied: a static table. NULL for none */
    const cyphal_register_t *registers;
    size_t register_count;
    /* Restart the board in delay_ms (by when the response has gone). NULL: restart and factory reset are
     * bad commands */
    void (*restart)(uint32_t delay_ms);
    /* A factory reset's part beyond the node's own settings (reset first): the application's settings to
     * their defaults, or, failing, left as they were; the node then puts its own back too and answers
     * internal error, with no restart. Then the restart. NULL: the node's settings only */
    esp_err_t (*factory_reset)(void);
    /* Show which board this is, for a few seconds, and return. NULL: identify is a bad command */
    esp_err_t (*identify)(void);
    /* identify() can take long (it shows things on a screen, say): it runs on the node's second worker, as a slow
     * handler's work does (cyphal_handler_t.slow), not on the worker */
    bool identify_slow;
    /* What the node does with its ports (cyphal_handler_t), one at most a port. Kept, not copied: a static
     * table. NULL for none */
    const cyphal_handler_t *handlers;
    size_t handler_count;
    /* Services of the application's whose IDs the standard fixes (uavcan.file.List, uavcan.file.GetInfo): no
     * register, served whenever the node is online, and in uavcan.node.port.List; each handler's fixed_id is its
     * service-ID, and the rest is a service's handler above (decode, work, busy, extent, in_size, slow).
     * CYPHAL_FIXED_SERVICES_MAX at most; one with an ID that is the node's own or another's is left out, with an
     * error in the log, and no application port may take its ID. Kept, not copied: a static table. NULL for none */
    const cyphal_handler_t *fixed_services;
    size_t fixed_service_count;
    /* The application's own work on the worker, between jobs: building what it publishes, carrying out what
     * it was sent. `online`: the node runs on its node-ID, so that cyphal_publish() goes out. Returns the
     * milliseconds until it wants to run again; cyphal_wake_worker() runs it sooner, and so does the node going
     * online. It runs from the node's first start on, whether or not the node is online. NULL for none */
    uint32_t (*poll)(bool online);
#if CONFIG_CYPHAL_CLIENT
    /* Another node's uavcan.node.Heartbeat (cyphal_heartbeat_t, cyphal_client.h): every one heard while the node is
     * online, read from its frame (cyphal_heartbeat_frame()), on the node's task, which it must not hold up: a copy,
     * and cyphal_wake_worker() for the work. A client's way to tell that a server restarted (its uptime going back)
     * or went quiet. Only a heartbeat of one frame is heard (the 1.0 type's 7 bytes; a later version taking more
     * than one frame would not be). With the hook set, port.List names 7509 among the subscribers. NULL for none.
     * The field exists only with CONFIG_CYPHAL_CLIENT */
    void (*on_heartbeat)(uint8_t from, const cyphal_heartbeat_t *hb);
#endif
} cyphal_app_t;

/* The saved settings, the register table and the lock. From app_main, once, before anything else
 * here; `app` (copied) may be NULL, for no ports and no commands. */
esp_err_t cyphal_init(const cyphal_app_t *app);

/* Start the node on these pins (TX, RX) with the saved settings: the controller at the saved
 * bitrate, then a fixed node-ID's listen, or plug-and-play's requests. The pins are the
 * caller's, checked as free, and the node's from then on. ESP_ERR_INVALID_STATE if it runs
 * already; another error if the controller would not start, which the status then shows: the
 * pins are then the caller's again, and cyphal_restart() won't take them. */
esp_err_t cyphal_start(int tx_gpio, int rx_gpio);

/* Stop, then start again on the same pins with the settings as they are now: also how a node
 * stopped by a node-ID in use starts again. ESP_ERR_INVALID_STATE if cyphal_start() never
 * succeeded (no pins to start on); any other error if the stop or the start failed, which the
 * status shows. */
esp_err_t cyphal_restart(void);

/* Settings, saved; each applies from the node's next start. A bad value is refused with
 * ESP_ERR_INVALID_ARG (and for a name, the reason in `why`); NVS's error if it wasn't saved. */
esp_err_t cyphal_set_node_id(uint8_t node_id);
esp_err_t cyphal_set_name(const char *name, char *why, size_t why_len);
esp_err_t cyphal_set_bitrate(uint32_t bitrate);
/* Back to the defaults (plug-and-play, CONFIG_CYPHAL_DEFAULT_NAME, 1 Mbit/s, every port's default
 * ID: every register), saved */
esp_err_t cyphal_reset_settings(void);

/* A register written (cyphal_registers.h's index and rules). One of the node's settings (the same as
 * above) is saved and applies from the node's next start; an application's register applies as its write
 * says. cyphal_reg_error()'s error for a refusal (the reason in `why`), or NVS's. `now` (NULL for none;
 * it may be `value` itself) gets the register's value as it is afterwards, written or not, as Access
 * answers a write; it is left alone for an index past the last. */
esp_err_t cyphal_write_register(size_t index, const uavcan_register_Value_1_0 *value, uavcan_register_Value_1_0 *now,
                                char *why, size_t why_len);

/* A register write's result as an error: ESP_OK, ESP_ERR_NOT_SUPPORTED for a register that can't be
 * written (or none), ESP_ERR_INVALID_ARG for a value refused */
esp_err_t cyphal_reg_error(cyphal_reg_result_t r);

void cyphal_status(cyphal_status_t *out);

/* A message on the application's port `port` (a CYPHAL_PORT_PUB with a handler), serialized: copied into its
 * mailbox for the node's task to send at the port's priority, in the order given. The message waits in the mailbox
 * until the transmit queue has room for the whole of it; the caller never waits. With depth 1 it replaces one not
 * yet sent. From any task. ESP_ERR_NOT_FINISHED when the mailbox is full: a queue's depth
 * waiting (nothing replaced: the caller learns it, and may try again), or, with depth 1, no buffer free because
 * another task is publishing on the port at the same moment. ESP_ERR_INVALID_STATE while the node isn't online
 * (nothing is kept), ESP_ERR_NOT_SUPPORTED for a port that is off this run, has no handler or isn't
 * published, ESP_ERR_INVALID_SIZE for more than its handler's publish_max. */
esp_err_t cyphal_publish(size_t port, const void *data, size_t size);

/* Run the application's poll on the worker soon (cyphal_app_t.poll): from any task, the node's included,
 * which it never holds up. Nothing before the node's first start */
void cyphal_wake_worker(void);

/* The health the heartbeat reports from now: CYPHAL_HEALTH_*. Nominal until set */
void cyphal_set_health(uint8_t health);

/* `cyphal`. From app_main, after cyphal_init(). `how_to_start`: the application's words for what
 * turns the node on, for the status while it's off (holo-player-fw's: "`link mode can`, then restart"). */
void cyphal_register_commands(const char *how_to_start);

#ifdef __cplusplus
}
#endif
