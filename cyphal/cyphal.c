/*
 * The Cyphal node. See cyphal.h, and the kit's README ("cyphal") for what it does.
 *
 * One task does everything libcanard is asked to do (libcanard is not thread-safe): it reads
 * frames from canbus's receive queue, runs the timers (the listen before going online, the
 * plug-and-play request, the heartbeat, a look at the bus for bus-off) and hands libcanard's
 * transmit queue to canbus one frame at a time, in libcanard's priority order. It sleeps until its
 * next timer or the next frame. The console only starts and stops it (under s_lock: a stop wakes
 * the task with canbus_wake() and waits for it to end) and reads counters.
 *
 * States: a start goes to LISTENING (a fixed node-ID) or WAITING (plug-and-play); LISTENING goes
 * ONLINE after CYPHAL_LISTEN_MS, or IN_USE if a frame from its node-ID arrives first; ONLINE goes
 * IN_USE the same way. IN_USE sends nothing until the next start. The controller runs in normal
 * mode throughout, so the board always acknowledges other nodes' frames, even while it sends
 * nothing.
 *
 * WAITING is anonymous: libcanard's node-ID is unset, the only subscription is to
 * uavcan.pnp.NodeIDAllocationData.1.0, and the only transfers sent are its requests, single frames
 * at random intervals, one queued at a time (the DSDL's rules A to D; cyphal_util.h). The node
 * listens from its start, as a fixed node-ID does, but for a heartbeat's longest period
 * (CYPHAL_PNP_LISTEN_US) and while it asks. An allocation for this board's pseudo-ID ends the
 * asking (rule D: no more requests, and no more allocations heard, so the first wins) and is held,
 * still WAITING, until that listen is over; then it goes ONLINE, unless the node-ID it gives has
 * been heard on the bus within a heartbeat's offline timeout (cyphal_pnp_verdict()): then IN_USE, as
 * a collision while running would be, without ever sending as that node-ID. Nothing is saved (D5).
 *
 * Nothing is sent before every frame already received has been checked for our node-ID (D9): the
 * task empties the receive queue before it hands a frame to canbus, every time, after the wait for
 * room and just before the hand-over. So a frame from another node with our node-ID stops the node
 * before its next frame; the one frame already inside canbus still goes. Taking up an allocated
 * node-ID is the same: the frames before that moment were counted as heard (cyphal_seen_t), and
 * those after it in the queue are checked for the new node-ID before anything goes out as it.
 * Both listens end by the frames' stamps, not by the clock (cyphal_listen_t): a task held off past
 * the end (a flash write stalls both cores) still checks the frames from before the end first.
 *
 * Sending: canbus is started with one send slot, so libcanard's queue goes to the controller one
 * frame at a time and its priority order holds; canbus_wait_room() waits for that slot and wakes
 * the task as soon as the frame before is on the bus. Every transfer gets a deadline
 * (TX_DEADLINE_US) when queued, applied before canbus_send() since canbus can't take a frame
 * back: a transfer whose deadline passes before its next frame is handed over is dropped whole,
 * wherever it waits in the queue (next_due()).
 * So one frame at most goes out late: the one inside canbus while nobody acknowledged it, or
 * while the controller was bus-off.
 *
 * Services: while online the node answers uavcan.node.GetInfo, uavcan.register.Access and List (the
 * register table is cyphal_registers.c's) and uavcan.node.ExecuteCommand, and publishes
 * uavcan.node.port.List every MAX_PUBLICATION_PERIOD (10 s), the first as it goes online. One table,
 * s_rows[], says what it publishes, subscribes to and serves: SERVICES[], the node's own fixed ports,
 * then a row for each of the application's ports that has a handler (cyphal_handler_t; cyphal_init()).
 * go_online() subscribes from it, a transfer finds its row through its subscription's user_reference,
 * and the port list is made from it, so the list can't disagree with what is subscribed. What it serves
 * and publishes is fixed for a run (the port registers apply from the next start), so that list is
 * serialized once a start, as GetInfo's response is.
 *
 * Work that can wait (a register written to NVS, or read where reading may wait: CYPHAL_REG_READ_ON_WORKER, a
 * restart, a factory reset, identify) is handed to
 * the worker, a task of its own (worker_task()), so that the node's task never blocks: libcanard
 * stays this task's alone, and so does every frame sent. The hand-over is a small fixed pool of job
 * slots (JOB_SLOTS, in PSRAM, their states in internal RAM for the atomics), for requests only. The
 * node's task decodes the request, takes a free slot, puts the decoded request in it with the
 * response's metadata, frees libcanard's buffer as usual, and queues the slot to the worker; the
 * worker does the work, serializes the response into the same slot, marks it done and wakes the node's
 * task with canbus_wake() (a marker in canbus's receive queue, which canbus_receive() skips); the
 * node's task pushes the response into libcanard's queue at its next turn (collect()), and only
 * flush() sends it, after the receive queue has been emptied, as everything else. The wake can be read
 * and skipped by flush()'s own drain() (a job finished while the task sends), so the task looks at the
 * slots again before it sleeps (jobs_done()), as it looks at s_running for a stop's wake. A job keeps the
 * start it came from, and its response is dropped if the node has restarted or stopped since. With no
 * free slot the request is answered at once: busy (ExecuteCommand's failure), or the register as it
 * is, unwritten; a register's read meant for the worker isn't answered (Access has no busy).
 *
 * The application's requests go the same way: its decode() on the node's task, into the slot (the
 * decode-once rule: the worker never sees the payload), its work() on the worker, which serializes the
 * response into the same slot; with no slot free, its busy() answers at once (or nothing does, for a type with no
 * way to say busy, whose busy() gives no response). Its subscriptions' messages
 * are handed to its on_message() on the node's task, which copies them into a mailbox of its own and wakes
 * the worker (cyphal_wake_worker(): a NULL in the work queue, one at most, for which the queue has a place
 * of its own); the worker then runs the application's poll(), which also runs on its own timer, between
 * jobs, to build what the application publishes.
 *
 * A request whose work can take long (its handler's `slow`, or identify when the application says so: on
 * holo-player-fw a file shown, which may decode an image, a scene applied, and identify, which applies a scene) goes
 * to a second worker instead, a task of its own made only when something needs it (slow_task()), by a queue of
 * its own, so that the worker's jobs and the application's poll (setpoints, what it publishes) never wait behind
 * an image. It takes job slots like any request, CYPHAL_SLOW_JOBS of them at most (s_slow_held), so that the
 * rest stay free for the worker's; with none to spare, the request is answered at once, busy, as with no slot
 * free.
 *
 * Messages the application publishes (cyphal_publish(), from any task) can't go by the slots: a
 * transfer-ID and the port are the node's task's to give, and publications would take the slots that
 * service calls need. Each published port has a mailbox (s_mail[]) of its handler's depth: 1 keeps the
 * newest message, replacing one not yet sent (counted as replaced); a deeper one is a queue that refuses a
 * message when full (counted as refused), for happenings, none of which may be lost unseen. A publisher
 * claims a buffer, fills it and commits it; the node's task takes the oldest committed one at its turn
 * (collect()), queues it straight from that buffer with the port, its priority (the port table's) and
 * the port's next transfer-ID, and frees it; a message the transmit queue hasn't room for, whole, waits in its
 * mailbox for a later turn (a queue's then fills, and refuses the next, which its publisher learns of), rather
 * than being dropped by libcanard. Only the claims, commits and takes are under the mailbox's
 * spinlock, never a copy; a count of committed buffers (s_mail_ready) says whether any wait without a lock.
 * Publishing wakes the node's task with canbus_wake().
 *
 * Memory: the node's state and libcanard's heap (CONFIG_CYPHAL_HEAP_SIZE, an IDF multi_heap) are
 * in PSRAM, allocated at the first start and kept; each start begins the heap empty, so a stop
 * needs to free nothing. So are the job slots, the worker's scratch and the mailboxes (cyphal_init()). The generated structs the
 * services need (a register request is 0.8 KB) are in node_t, the job slots or the worker's scratch,
 * never on a stack. Only the two tasks' stacks and the queues canbus owns are internal RAM; the worker,
 * made at the first start, is kept.
 */
#include <stddef.h>
#include <string.h>
#include "canard.h"
#include "canbus.h"
#include "cyphal.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "multi_heap.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "uavcan/_register/Access_1_0.h"
#include "uavcan/_register/List_1_0.h"
#include "uavcan/node/ExecuteCommand_1_3.h"
#include "uavcan/node/GetInfo_1_0.h"
#include "uavcan/node/Heartbeat_1_0.h"
#include "uavcan/node/port/List_1_0.h"
#include "uavcan/pnp/NodeIDAllocationData_1_0.h"
#if CONFIG_CYPHAL_PNP_ALLOCATOR
#include <stdio.h>
#include "cyphal_alloc.h"
#endif

static const char *TAG = "cyphal";

#define NVS_NS          "cyphal"
#define KEY_NODE_ID     CYPHAL_NVS_KEY_NODE_ID  /* u8: 0-127, or 255 for plug-and-play */
#define KEY_NAME        CYPHAL_NVS_KEY_NAME     /* str */
#define KEY_BITRATE     CYPHAL_NVS_KEY_BITRATE  /* u32 */
                                        /* and each application port's ID: u16, under its nvs_key ("pub.status") */

#define TX_QUEUE_FRAMES     160         /* libcanard's transmit queue; see CONFIG_CYPHAL_HEAP_SIZE */
#define HEARTBEAT_US        1000000
#define TX_DEADLINE_US      1000000     /* a heartbeat period, for every transfer */
#define BUS_CHECK_US        1000000     /* how often the task looks for bus-off */
#define RX_BUDGET           64          /* frames read before the timers get a look (nothing is sent until the queue is empty) */
#define PORT_LIST_US        ((int64_t)uavcan_node_port_List_1_0_MAX_PUBLICATION_PERIOD * 1000000)
#define PORT_LIST_MAX       256         /* the port list, serialized: 152 bytes now, with room for the application's ports */
#define JOB_SLOTS           4           /* requests in the worker's hands at once; a fifth is answered busy */
/* The largest response the worker serializes: a register's Access */
#define JOB_BYTES           uavcan_register_Access_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_
/* A restart (or a factory reset's) waits for the response to go: by its deadline it has, or been dropped */
#define RESTART_AFTER_MS    (TX_DEADLINE_US / 1000)

_Static_assert(sizeof(CONFIG_CYPHAL_UNIQUE_ID_PREFIX) == CYPHAL_UID_PREFIX_SIZE + 1,
               "CONFIG_CYPHAL_UNIQUE_ID_PREFIX is exactly 8 characters");
_Static_assert(uavcan_node_ExecuteCommand_Response_1_3_SERIALIZATION_BUFFER_SIZE_BYTES_ <= JOB_BYTES,
               "a response fits the slot its request came in");

/* The responses the node's task serializes itself: the largest of them */
#define OUT_BYTES uavcan_register_Access_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_
_Static_assert(uavcan_register_List_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_ <= OUT_BYTES &&
               uavcan_node_ExecuteCommand_Response_1_3_SERIALIZATION_BUFFER_SIZE_BYTES_ <= OUT_BYTES, "node_t.out");

typedef struct node node_t;
typedef struct job job_t;
typedef struct service service_t;

/* A row's part on the node's task, for a transfer to its port (now: its arrival's stamp) */
typedef void (*serve_fn)(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
/* The worker's part of a request (job->in), its response serialized into job->data (job->size its size;
 * 0 for none). True to restart the board once the response has gone */
typedef bool (*work_fn)(job_t *job);

/* One of the node's ports while online: what it publishes (listed; sent by its timer, or from its mailbox),
 * subscribes to or serves (subscribed, and answered by serve). An application's port is a row with app_port
 * and app set */
struct service {
    cyphal_port_kind_t kind;
    uint16_t port_id;                       /* a fixed port's */
    int8_t app_port;                        /* or the application's port (cyphal_port()), whose ID is the run's
                                             * s_run.port_id[app_port]: none while it is CYPHAL_PORT_OFF; -1 for fixed */
    size_t extent;                          /* the request's or message's, which libcanard cuts a transfer to */
    serve_fn serve;                         /* NULL for a subject published */
    work_fn work;                           /* the worker's part, for serve to hand over; NULL for none */
    const cyphal_handler_t *app;            /* an application's port's handler; NULL for a fixed port */
};

static void answer_getinfo(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_access(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_list(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_command(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_allocation(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_app_request(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static void on_app_message(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);
static bool work_access(job_t *job);
static bool work_command(job_t *job);
static bool work_app(job_t *job);

static const service_t SERVICES[] = {
    { CYPHAL_PORT_PUB, uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, -1, 0, NULL, NULL, NULL },
    { CYPHAL_PORT_PUB, uavcan_node_port_List_1_0_FIXED_PORT_ID_, -1, 0, NULL, NULL, NULL },
    { CYPHAL_PORT_SRV, uavcan_node_GetInfo_1_0_FIXED_PORT_ID_, -1, uavcan_node_GetInfo_Request_1_0_EXTENT_BYTES_,
      answer_getinfo, NULL, NULL },
    { CYPHAL_PORT_SRV, uavcan_register_Access_1_0_FIXED_PORT_ID_, -1, uavcan_register_Access_Request_1_0_EXTENT_BYTES_,
      on_access, work_access, NULL },
    { CYPHAL_PORT_SRV, uavcan_register_List_1_0_FIXED_PORT_ID_, -1, uavcan_register_List_Request_1_0_EXTENT_BYTES_,
      on_list, NULL, NULL },
    { CYPHAL_PORT_SRV, uavcan_node_ExecuteCommand_1_3_FIXED_PORT_ID_, -1,
      uavcan_node_ExecuteCommand_Request_1_3_EXTENT_BYTES_, on_command, work_command, NULL },
};
#define SERVICE_COUNT (sizeof(SERVICES) / sizeof(SERVICES[0]))
/* Every row the node can have: its own, one for each of the application's ports, and its fixed services */
#define ROW_MAX       (SERVICE_COUNT + CYPHAL_PORTS_MAX + CYPHAL_FIXED_SERVICES_MAX)
_Static_assert(ROW_MAX <= uavcan_node_port_SubjectIDList_1_0_sparse_list_ARRAY_CAPACITY_,
               "the port list holds every subject the node can have");

/* SERVICES[], then the application's ports with handlers (cyphal_init()): fixed from then on */
static service_t s_rows[ROW_MAX];
static size_t s_row_count;

/* Plug-and-play's, while waiting only: not a port the node has online, so not in s_rows[] or the list */
static const service_t ALLOCATION = { CYPHAL_PORT_SUB, uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_, -1,
                                      uavcan_pnp_NodeIDAllocationData_1_0_EXTENT_BYTES_, on_allocation, NULL, NULL };

/* A published port's mailbox (cyphal_publish()): its depth of messages waiting for the node's task, in depth + 1
 * buffers, so that one can be on its way out while the rest wait. A buffer goes FREE, FILLING (a publisher's),
 * READY (committed, in commit order), SENDING (the node task's, queued straight from it), FREE again */
enum { BUF_FREE, BUF_FILLING, BUF_READY, BUF_SENDING };
#define MAIL_BUFS (CYPHAL_PUBLISH_DEPTH_MAX + 1)
typedef struct {
    portMUX_TYPE mux;                       /* the buffers' states, sizes and order, and the counts */
    size_t port;                            /* the application's port */
    size_t max;                             /* its handler's publish_max: each buffer's size */
    uint8_t depth;                          /* its handler's publish_depth: 1, the newest */
    uint8_t state[MAIL_BUFS];               /* depth + 1 of them in use */
    uint32_t order[MAIL_BUFS];              /* a READY buffer's place in the commit order */
    size_t size[MAIL_BUFS];                 /* a READY or SENDING buffer's message */
    uint8_t *data;                          /* (depth + 1) * max bytes, PSRAM where there is some, from cyphal_init() */
    uint32_t next_order;
    uint32_t ready;                         /* READY buffers: written under mux, read through __atomic without it */
    uint32_t published, replaced, refused;  /* since the start (cyphal_mail_t) */
    CanardTransferID tid;                   /* the node task's: the port's next transfer-ID, from 0 at each start */
} mailbox_t;
static mailbox_t *s_mail;                   /* one for each published port with a handler, internal RAM (their spinlocks), from
                                             * cyphal_init(); never freed */
static size_t s_mail_count;
static uint32_t s_mail_ready;               /* READY buffers in every mailbox, the same way: mail_waiting() */

struct node {
    struct CanardInstance canard;
    struct CanardTxQueue txq;
    /* s_rows[]' subscriptions while online, and NodeIDAllocationData's while waiting, each with its row
     * as user_reference. They must not move while subscribed: s_node is never freed */
    struct CanardRxSubscription subs[ROW_MAX];
    struct CanardRxSubscription allocation;
    uint32_t run;                           /* this start's number (s_runs), which its jobs carry */
    uint8_t getinfo_response[uavcan_node_GetInfo_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t getinfo_response_size;           /* serialized once per start: nothing in it changes */
    uint8_t port_list[PORT_LIST_MAX];       /* uavcan.node.port.List, serialized once per start: the same */
    size_t port_list_size;
    CanardTransferID port_list_tid;
    int64_t next_port_list_us;
    /* The services' structs the task decodes and answers with: here, off its stack */
    union {
        uavcan_register_Access_Request_1_0 access;
        uavcan_node_ExecuteCommand_Request_1_3 command;
    } rq;
    union {
        uavcan_register_Access_Response_1_0 access;
        uavcan_register_List_Response_1_0 list;
        uavcan_node_ExecuteCommand_Response_1_3 command;
    } rs;
    uint8_t out[OUT_BYTES];                 /* a response serialized, for push(), which copies it */
    uint8_t node_id;                        /* the one it runs, or listens, on; CYPHAL_NODE_ID_AUTO while waiting */
    uint8_t allocator;                      /* the node that gave it, with plug-and-play (s_run.node_id AUTO) */
    uint8_t held_id;                        /* allocated, while the listen goes on; CYPHAL_NODE_ID_AUTO for none */
    int64_t held_us;                        /* the allocation's stamp */
    CanardTransferID heartbeat_tid, request_tid;
    cyphal_listen_t listen;                 /* from the start: a fixed node-ID's before going online, plug-and-play's before taking one up */
    cyphal_seen_t seen;                     /* when each node-ID was last heard while waiting: an allocation's check */
    int64_t next_request_us;                /* plug-and-play's request timer (Trequest), while waiting with nothing held */
    int64_t next_heartbeat_us, next_bus_check_us;
    uint32_t bus_off_seen;                  /* canbus's bus-off count, as last looked at */
    cyphal_busoff_t busoff;                 /* the recoveries' back-off */
    int64_t recover_at_us;                  /* when to ask for the recovery, while bus-off */
};

/* A request in the worker's hands, decoded, and then its response */
struct job {
    uint32_t run;                           /* the node's start it came in (node_t.run) */
    bool slow;                              /* the second worker's (slow_task()), counted in s_slow_held */
    work_fn work;
    const cyphal_handler_t *app;            /* an application's request's handler; NULL for the node's own */
    struct CanardTransferMetadata meta;     /* the response's */
    union {                                 /* the request, as the node's task decoded it */
        struct {
            size_t index;
            uavcan_register_Value_1_0 value;
        } write;                            /* Access: a register and its new value */
        uavcan_node_ExecuteCommand_Request_1_3 command;
        _Alignas(8) uint8_t app[CYPHAL_APP_REQUEST_MAX];    /* an application's, by its decode() */
    } in;
    size_t size;                            /* the response's */
    uint8_t data[JOB_BYTES];                /* the response, serialized */
};
/* A slot's state, s_job_state[]: FREE, taken by whoever fills it (FILLING), the worker's (WORK), and
 * done, for the node's task to send and free (DONE). Only a compare-and-swap takes a FREE slot */
enum { JOB_FREE, JOB_FILLING, JOB_WORK, JOB_DONE };

/* The worker's scratch: the Access response it builds, off its stack (work_access(), which runs on the worker alone,
 * one job at a time). The second worker has none: what it runs (a slow handler's work, identify's command response,
 * work_command()) builds its response on its own stack, so the two never write the same memory */
typedef union {
    uavcan_register_Access_Response_1_0 access;
} worker_scratch_t;

static SemaphoreHandle_t s_lock;        /* settings, start, stop and status against each other */
static SemaphoreHandle_t s_task_ended;  /* given by the task as it ends */
static cyphal_app_t s_app;              /* the application's ports, registers and commands (cyphal_init()) */
static cyphal_settings_t s_saved;       /* from the next start: written under s_lock and s_saved_mux both */
static portMUX_TYPE s_saved_mux = portMUX_INITIALIZER_UNLOCKED;     /* s_saved for the node's task, which never takes s_lock */
static cyphal_settings_t s_run;         /* as the node started */
static int s_tx = -1, s_rx = -1;        /* the pins of the last start */
static bool s_given;                    /* cyphal_start() succeeded on them, so they are the node's to restart on */
static uint8_t s_uid[CYPHAL_UNIQUE_ID_SIZE];
static uint64_t s_pseudo_id;            /* plug-and-play's, from s_uid */
static node_t *s_node;                  /* from the first start; never freed */
static void *s_heap_mem;                /* CONFIG_CYPHAL_HEAP_SIZE bytes, from the first start */
static multi_heap_handle_t s_heap;      /* in s_heap_mem, made again at each start */
static size_t s_heap_free0;             /* its free size when empty */
static canbus_handle_t s_bus;           /* the controller's run, from a start to its stop; 0 for none */
static TaskHandle_t s_task;
static uint32_t s_runs;                 /* starts so far, under s_lock: node_t.run */
static job_t *s_jobs;                   /* JOB_SLOTS of them, from the first start; never freed */
static uint32_t s_job_state[JOB_SLOTS]; /* JOB_FREE..JOB_DONE, through __atomic only */
static QueueHandle_t s_work_q;          /* job_t *, to the worker, or NULL for a wake (cyphal_wake_worker()): JOB_SLOTS + 1
                                         * long, so never full */
static uint32_t s_wake_queued;          /* a wake is in s_work_q, not yet taken: one at most. Through __atomic only */
static volatile uint8_t s_health = CYPHAL_HEALTH_NOMINAL;   /* the heartbeat's (cyphal_set_health()) */
static TaskHandle_t s_worker;           /* from the first start; never deleted */
static bool s_has_slow;                 /* a handler or identify is slow (cyphal_handler_t, cyphal_app_t), so the
                                         * second worker is made */
static QueueHandle_t s_slow_q;          /* job_t *, to the second worker: CYPHAL_SLOW_JOBS long, from the first start */
static TaskHandle_t s_slow;             /* the second worker, from the first start when s_has_slow; never deleted */
static uint32_t s_slow_held;            /* job slots taken for it and not yet done: through __atomic only */
static volatile uint32_t s_txq_peak;    /* the most frames in libcanard's transmit queue since the start */
static worker_scratch_t *s_ws;          /* the worker's */
static volatile bool s_running;         /* the task's to go on; a stop clears it */
static volatile cyphal_state_t s_state = CYPHAL_OFF;
static volatile esp_err_t s_error;
static bool s_stop_failed;              /* CYPHAL_FAILED came from a restart's canbus_stop(); under s_lock */
static volatile int64_t s_started_us;
static volatile uint32_t s_recover_at_ms;   /* bus-off: when the node asks for the recovery, esp_timer ms; 0 for none waiting */
/* Since the start; written by the task (s_services by the worker too), read whole by anyone */
static volatile uint32_t s_tr_sent, s_tr_received, s_tr_dropped, s_frames_queued, s_heap_failed;
static volatile cyphal_services_t s_services;

#if CONFIG_CYPHAL_PNP_ALLOCATOR
/* The plug-and-play allocator (cyphal_alloc.h): its code is at the end of this file; these are where the node calls it */
static esp_err_t alloc_reset(void);
static esp_err_t alloc_reset_locked(void);
static bool alloc_reg_init(char *why, size_t why_len);
static void alloc_load(void);
static void alloc_start(void);
static void alloc_online(node_t *n);
static void alloc_in_use(void);
static void alloc_stop(void);
static void alloc_worker(void);
static void alloc_frame(node_t *n, const canbus_frame_t *f, cyphal_state_t state, int64_t stamp);
static esp_err_t alloc_port_lists(node_t *n);
static const uint8_t *alloc_port_list(const node_t *n, size_t *size);
static bool alloc_is_register(const cyphal_register_t *r);
static bool s_alloc_port_listed;    /* serialize_port_list() adds the allocator's subject (alloc_port_lists()) */
#endif

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ settings */

static void defaults(cyphal_settings_t *s)
{
    *s = (cyphal_settings_t){ .node_id = CYPHAL_NODE_ID_AUTO, .bitrate = CANBUS_DEFAULT_BITRATE };
    cyphal_ports_default(s);
    char why[96];
    if (cyphal_name_valid(CONFIG_CYPHAL_DEFAULT_NAME, why, sizeof(why))) {
        strlcpy(s->name, CONFIG_CYPHAL_DEFAULT_NAME, sizeof(s->name));
    } else {
        ESP_LOGW(TAG, "CONFIG_CYPHAL_DEFAULT_NAME is no name (%s): \"node\" instead", why);
        strlcpy(s->name, "node", sizeof(s->name));
    }
}

/* All of them, every time: a backup of a board that has changed one holds them all */
static esp_err_t save(const cyphal_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_NODE_ID, s->node_id);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_NAME, s->name);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, KEY_BITRATE, s->bitrate);
    }
    for (size_t i = 0; err == ESP_OK && i < cyphal_port_count(); i++) {
        err = nvs_set_u16(h, cyphal_port(i)->nvs_key, s->port_id[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* What was saved, over the defaults: a value that doesn't read is skipped, with a warning */
static void load(cyphal_settings_t *s)
{
    defaults(s);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t id;
    if (nvs_get_u8(h, KEY_NODE_ID, &id) == ESP_OK) {
        if (cyphal_node_id_valid(id)) {
            s->node_id = id;
        } else {
            ESP_LOGW(TAG, "saved node-ID %u isn't one: plug-and-play", id);
        }
    }
    char name[CYPHAL_NAME_MAX + 1], why[96];
    size_t len = sizeof(name);
    esp_err_t err = nvs_get_str(h, KEY_NAME, name, &len);
    if (err == ESP_OK && cyphal_name_valid(name, why, sizeof(why))) {
        strlcpy(s->name, name, sizeof(s->name));
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "the saved name doesn't do (%s): %s", err == ESP_OK ? why : esp_err_to_name(err), s->name);
    }
    uint32_t bitrate;
    if (nvs_get_u32(h, KEY_BITRATE, &bitrate) == ESP_OK) {
        if (cyphal_bitrate_valid(bitrate)) {
            s->bitrate = bitrate;
        } else {
            ESP_LOGW(TAG, "saved bitrate %lu isn't one of 125k, 250k, 500k, 1M: 1M", (unsigned long)bitrate);
        }
    }
    for (size_t i = 0; i < cyphal_port_count(); i++) {
        const cyphal_port_t *p = cyphal_port(i);
        uint16_t port_id;
        if (nvs_get_u16(h, p->nvs_key, &port_id) != ESP_OK) {
            continue;
        }
        if (cyphal_port_id_valid(p->kind, port_id)) {
            s->port_id[i] = port_id;
        } else {
            ESP_LOGW(TAG, "saved %s %u isn't one: %u", p->id_register, port_id, p->default_id);
        }
    }
    nvs_close(h);
    /* No write lets two ports share an ID, or take one of the node's own, but a configuration from
     * elsewhere can: the later port is turned off, and its register reads so */
    const cyphal_settings_t was = *s;
    const uint32_t off = cyphal_ports_settle(s);
    for (size_t i = 0; i < cyphal_port_count(); i++) {
        if (off & (1u << i)) {
            ESP_LOGW(TAG, "saved %s %u is taken (by one of the node's own ports, or an earlier port): off",
                     cyphal_port(i)->id_register, was.port_id[i]);
        }
    }
}

/* The ports fixed by the standard, for the port registers' rule (cyphal_reg_fixed()): SERVICES[]' rows,
 * plug-and-play's, and the application's fixed services that can be kept (s_app_fixed[]) */
static cyphal_fixed_port_t s_fixed[SERVICE_COUNT + 1 + CYPHAL_FIXED_SERVICES_MAX];
static const cyphal_handler_t *s_app_fixed[CYPHAL_FIXED_SERVICES_MAX];
static size_t s_app_fixed_count;

/* An application's fixed service's fault, if it can't be kept beside the n ports fixed so far: NULL when it can */
static const char *fixed_fault(const cyphal_handler_t *h, size_t n)
{
    if (s_app_fixed_count >= CYPHAL_FIXED_SERVICES_MAX) {
        return "more than CYPHAL_FIXED_SERVICES_MAX";
    }
    if (h->fixed_id > CYPHAL_SERVICE_ID_MAX) {
        return "not a service-ID";
    }
    for (size_t i = 0; i < n; i++) {
        if (s_fixed[i].kind == CYPHAL_PORT_SRV && s_fixed[i].id == h->fixed_id) {
            return "its service-ID is taken";
        }
    }
    return h->decode == NULL || h->work == NULL || h->busy == NULL ? "a service needs decode, work and busy"
           : h->in_size > CYPHAL_APP_REQUEST_MAX ? "its request is over CYPHAL_APP_REQUEST_MAX"
           : h->extent == 0 ? "no extent" : NULL;
}

static void fix_ports(void)
{
    size_t n = 0;
    for (size_t i = 0; i < SERVICE_COUNT; i++) {
        if (SERVICES[i].app_port < 0) {
            s_fixed[n++] = (cyphal_fixed_port_t){ SERVICES[i].kind, SERVICES[i].port_id };
        }
    }
    s_fixed[n++] = (cyphal_fixed_port_t){ ALLOCATION.kind, ALLOCATION.port_id };
    s_app_fixed_count = 0;
    for (size_t i = 0; s_app.fixed_services != NULL && i < s_app.fixed_service_count; i++) {
        const cyphal_handler_t *h = &s_app.fixed_services[i];
        const char *fault = fixed_fault(h, n);
        if (fault != NULL) {
            ESP_LOGE(TAG, "the application's service %u: %s; left out", h->fixed_id, fault);
            continue;
        }
        s_app_fixed[s_app_fixed_count++] = h;
        s_fixed[n++] = (cyphal_fixed_port_t){ CYPHAL_PORT_SRV, h->fixed_id };
    }
    cyphal_reg_fixed(s_fixed, n);
}

/* New saved settings, under s_lock: in NVS first, then in use from the next start */
static esp_err_t commit(const cyphal_settings_t *s)
{
    const esp_err_t err = save(s);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_saved_mux);
        s_saved = *s;
        taskEXIT_CRITICAL(&s_saved_mux);
    }
    return err;
}

/* The saved settings, for the node's task: under the spinlock, never s_lock, which a stop holds while it
 * waits for the task to end */
static void saved_copy(cyphal_settings_t *out)
{
    taskENTER_CRITICAL(&s_saved_mux);
    *out = s_saved;
    taskEXIT_CRITICAL(&s_saved_mux);
}

esp_err_t cyphal_reg_error(cyphal_reg_result_t r)
{
    return r == CYPHAL_REG_OK ? ESP_OK : r == CYPHAL_REG_IMMUTABLE ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
}

/* A change to the settings: checked (CYPHAL_REG_OK, or a refusal with why) and made in *s */
typedef cyphal_reg_result_t (*change_fn)(cyphal_settings_t *s, const void *arg, char *why, size_t why_len);

/* The one way the saved settings change: a copy changed and committed, all under s_lock, so that two
 * writers (the console, the worker) never lose one another's change */
static esp_err_t update(change_fn change, const void *arg, char *why, size_t why_len)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    cyphal_settings_t s = s_saved;
    const cyphal_reg_result_t r = change(&s, arg, why, why_len);
    const esp_err_t err = r == CYPHAL_REG_OK ? commit(&s) : cyphal_reg_error(r);
    unlock();
    return err;
}

/* The setters' changes, checked before (each rule is cyphal_util's or the register table's) */
static cyphal_reg_result_t put_node_id(cyphal_settings_t *s, const void *arg, char *why, size_t why_len)
{
    (void)why;
    (void)why_len;
    s->node_id = *(const uint8_t *)arg;
    return CYPHAL_REG_OK;
}

static cyphal_reg_result_t put_name(cyphal_settings_t *s, const void *arg, char *why, size_t why_len)
{
    (void)why;
    (void)why_len;
    strlcpy(s->name, arg, sizeof(s->name));
    return CYPHAL_REG_OK;
}

static cyphal_reg_result_t put_bitrate(cyphal_settings_t *s, const void *arg, char *why, size_t why_len)
{
    (void)why;
    (void)why_len;
    s->bitrate = *(const uint32_t *)arg;
    return CYPHAL_REG_OK;
}

static cyphal_reg_result_t put_defaults(cyphal_settings_t *s, const void *arg, char *why, size_t why_len)
{
    (void)arg;
    (void)why;
    (void)why_len;
    defaults(s);
    return CYPHAL_REG_OK;
}

typedef struct {
    size_t index;
    const uavcan_register_Value_1_0 *value;
} reg_write_t;

static cyphal_reg_result_t put_register(cyphal_settings_t *s, const void *arg, char *why, size_t why_len)
{
    const reg_write_t *w = arg;
    return cyphal_reg_write(s, w->index, w->value, why, why_len);
}

esp_err_t cyphal_set_node_id(uint8_t node_id)
{
    return cyphal_node_id_valid(node_id) ? update(put_node_id, &node_id, NULL, 0) : ESP_ERR_INVALID_ARG;
}

esp_err_t cyphal_set_name(const char *name, char *why, size_t why_len)
{
    return cyphal_name_valid(name, why, why_len) ? update(put_name, name, why, why_len) : ESP_ERR_INVALID_ARG;
}

esp_err_t cyphal_set_bitrate(uint32_t bitrate)
{
    return cyphal_bitrate_valid(bitrate) ? update(put_bitrate, &bitrate, NULL, 0) : ESP_ERR_INVALID_ARG;
}

esp_err_t cyphal_reset_settings(void)
{
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    /* The allocator's setting off and its table empty, last: if the node's reset fails, the table is as it was. If
     * the allocator's fails, the node's settings stay reset and its error is returned */
    const esp_err_t err = update(put_defaults, NULL, NULL, 0);
    return err == ESP_OK ? alloc_reset() : err;
#else
    return update(put_defaults, NULL, NULL, 0);
#endif
}

/* uavcan.node.ExecuteCommand's factory reset, all or nothing: the node's settings to their defaults, then
 * the application's (s_app.factory_reset, which leaves its own as they were if it fails); if that fails,
 * the node's are put back as they were. Under s_lock throughout, so that no other write comes between */
static esp_err_t factory_reset(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const cyphal_settings_t was = s_saved;
    cyphal_settings_t s;
    defaults(&s);
    esp_err_t err = commit(&s);
    if (err == ESP_OK && s_app.factory_reset != NULL) {
        err = s_app.factory_reset();
        if (err != ESP_OK) {
            const esp_err_t back = commit(&was);
            if (back != ESP_OK) {
                ESP_LOGE(TAG, "factory reset: the node's settings couldn't be put back (%s): they are at their "
                         "defaults", esp_err_to_name(back));
            }
        }
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    /* The allocator's setting off and its table empty, last, so that a failure of the node's or the application's
     * reset leaves the table as it was. If this part fails, the node's and the application's resets stand (the
     * application's can't be undone), the table and the setting are as they were, and the error is answered: no
     * restart */
    if (err == ESP_OK) {
        err = alloc_reset_locked();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory reset: the allocator's table and setting weren't reset (%s); the node's and the "
                     "application's settings were", esp_err_to_name(err));
        }
    }
#endif
    unlock();
    return err;
}

esp_err_t cyphal_write_register(size_t index, const uavcan_register_Value_1_0 *value, uavcan_register_Value_1_0 *now,
                                char *why, size_t why_len)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const cyphal_register_t *r = cyphal_reg(index);
    esp_err_t err;
    if (r != NULL && (r->flags & CYPHAL_REG_NEXT_START)) {
        const reg_write_t w = { index, value };
        err = update(put_register, &w, why, why_len);
    } else {
        lock();     /* the application's own, kept and applied by it (or no register): one write at a time too */
        err = cyphal_reg_error(cyphal_reg_write(NULL, index, value, why, why_len));
        unlock();
    }
    if (now != NULL && r != NULL) {
        cyphal_settings_t s;
        saved_copy(&s);
        cyphal_reg_read(&s, index, now);
    }
    return err;
}

static void *psram_calloc(size_t size);

/* The handler's fault, if it can't be kept: NULL when it can. Its port is in the table by now */
static const char *handler_fault(const cyphal_handler_t *h, const bool taken[CYPHAL_PORTS_MAX])
{
    const cyphal_port_t *p = cyphal_port(h->port);
    if (p == NULL) {
        return "no such port";
    }
    if (taken[h->port]) {
        return "a second handler for its port";
    }
    switch (p->kind) {
    case CYPHAL_PORT_SRV:
        return h->decode == NULL || h->work == NULL || h->busy == NULL ? "a service needs decode, work and busy"
               : h->in_size > CYPHAL_APP_REQUEST_MAX ? "its request is over CYPHAL_APP_REQUEST_MAX"
               : h->extent == 0 ? "no extent" : NULL;
    case CYPHAL_PORT_SUB:
        return h->on_message == NULL ? "a subscription needs on_message" : h->extent == 0 ? "no extent" : NULL;
    default:
        return h->publish_max == 0 || h->publish_max > CYPHAL_PUBLISH_MAX ? "publish_max is 1 to CYPHAL_PUBLISH_MAX"
               : h->publish_depth > CYPHAL_PUBLISH_DEPTH_MAX ? "publish_depth is over CYPHAL_PUBLISH_DEPTH_MAX"
               : NULL;
    }
}

/* s_rows[]: SERVICES[], then a row for each handler the node can keep, with a mailbox for each published one.
 * A handler it can't keep is left out, with an error in the log */
static void make_rows(void)
{
    memcpy(s_rows, SERVICES, sizeof(SERVICES));
    s_row_count = SERVICE_COUNT;
    s_has_slow = s_app.identify != NULL && s_app.identify_slow;
    size_t published = 0;
    for (size_t i = 0; i < s_app.handler_count; i++) {
        const cyphal_port_t *p = cyphal_port(s_app.handlers[i].port);
        published += p != NULL && p->kind == CYPHAL_PORT_PUB;
    }
    if (published > 0) {
        s_mail = heap_caps_calloc(published, sizeof(mailbox_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    bool taken[CYPHAL_PORTS_MAX] = { false };
    for (size_t i = 0; i < s_app.handler_count; i++) {
        const cyphal_handler_t *h = &s_app.handlers[i];
        const char *fault = handler_fault(h, taken);
        const cyphal_port_t *p = cyphal_port(h->port);
        if (fault == NULL && p->kind == CYPHAL_PORT_PUB) {
            mailbox_t *m = s_mail != NULL ? &s_mail[s_mail_count] : NULL;
            const uint8_t depth = h->publish_depth > 1 ? h->publish_depth : 1;
            if (m != NULL) {
                m->data = psram_calloc((size_t)(depth + 1) * h->publish_max);
            }
            if (m == NULL || m->data == NULL) {
                fault = "no memory for its mailbox";
            } else {
                portMUX_INITIALIZE(&m->mux);
                m->port = h->port;
                m->max = h->publish_max;
                m->depth = depth;
                s_mail_count++;
            }
        }
        if (fault != NULL) {
            ESP_LOGE(TAG, "the application's handler for %s: %s; left out", p != NULL ? p->id_register : "?", fault);
            continue;
        }
        taken[h->port] = true;
        s_has_slow |= p->kind == CYPHAL_PORT_SRV && h->slow;
        s_rows[s_row_count++] = (service_t){
            .kind = p->kind,
            .app_port = (int8_t)h->port,
            .extent = h->extent,
            .serve = p->kind == CYPHAL_PORT_SRV ? on_app_request : p->kind == CYPHAL_PORT_SUB ? on_app_message : NULL,
            .work = p->kind == CYPHAL_PORT_SRV ? work_app : NULL,
            .app = h,
        };
    }
    /* The application's services on fixed IDs, checked by fix_ports() */
    for (size_t i = 0; i < s_app_fixed_count; i++) {
        const cyphal_handler_t *h = s_app_fixed[i];
        s_has_slow |= h->slow;
        s_rows[s_row_count++] = (service_t){
            .kind = CYPHAL_PORT_SRV, .port_id = h->fixed_id, .app_port = -1, .extent = h->extent,
            .serve = on_app_request, .work = work_app, .app = h,
        };
    }
}

esp_err_t cyphal_init(const cyphal_app_t *app)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_task_ended = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_task_ended == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (app != NULL) {
        s_app = *app;
    }
    char why[128];
    fix_ports();
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    if (!alloc_reg_init(why, sizeof(why))) {    /* the application's registers, and the allocator's after them */
#else
    if (!cyphal_reg_init(s_app.ports, s_app.port_count, CONFIG_CYPHAL_NAME_REGISTER, s_app.registers,
                         s_app.register_count, why, sizeof(why))) {
#endif
        ESP_LOGE(TAG, "the application's ports and registers: %s; none kept", why);
    }
    make_rows();
    load(&s_saved);
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_load();
#endif
    /* The factory MAC, burnt into eFuse: the same after a reflash or a settings wipe */
    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no factory MAC (%s): the unique ID ends in zeros", esp_err_to_name(err));
    }
    cyphal_unique_id(s_uid, CONFIG_CYPHAL_UNIQUE_ID_PREFIX, mac);
    s_pseudo_id = cyphal_pseudo_id(s_uid);
    return ESP_OK;
}

/* ------------------------------------------------------------------ libcanard's heap */

/* libcanard asks for max_align_t, 8 bytes here: the uint64_t in its queue items. It asks for 0 bytes for
 * an anonymous frame with nothing but its tail byte (canard.c, rxAcceptFrame) and takes NULL as out of
 * memory, which multi_heap gives for 0: so a byte instead, a block heap_free() frees like any other */
static void *heap_alloc(void *ref, size_t size)
{
    (void)ref;
    void *p = multi_heap_aligned_alloc(s_heap, size > 0 ? size : 1, 8);
    if (p == NULL) {
        s_heap_failed++;
    }
    return p;
}

static void heap_free(void *ref, size_t size, void *p)
{
    (void)ref;
    (void)size;
    multi_heap_free(s_heap, p);
}

static void worker_task(void *arg);
static void slow_task(void *arg);

static void *psram_calloc(size_t size)
{
    return heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

/* The node's state and heap: PSRAM if there is any, at the first start; emptied at each. The job
 * slots and the worker, at the first start, kept: a slot the worker still holds at a stop is done,
 * and dropped, at the next start's turn (collect()) */
static esp_err_t make_node(void)
{
    if (s_node == NULL) {
        s_node = heap_caps_malloc_prefer(sizeof(node_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    }
    if (s_heap_mem == NULL) {
        s_heap_mem = heap_caps_malloc_prefer(CONFIG_CYPHAL_HEAP_SIZE, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                             MALLOC_CAP_8BIT);
    }
    if (s_jobs == NULL) {
        s_jobs = psram_calloc(JOB_SLOTS * sizeof(job_t));
    }
    if (s_ws == NULL) {
        s_ws = psram_calloc(sizeof(worker_scratch_t));
    }
    if (s_work_q == NULL) {
        s_work_q = xQueueCreate(JOB_SLOTS + 1, sizeof(job_t *));     /* every slot's job, and a wake */
    }
    if (s_node == NULL || s_heap_mem == NULL || s_jobs == NULL || s_ws == NULL || s_work_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_worker == NULL && xTaskCreatePinnedToCore(worker_task, "cyphal_work", CONFIG_CYPHAL_WORKER_STACK, NULL,
                                                    CONFIG_CYPHAL_WORKER_PRIORITY, &s_worker, tskNO_AFFINITY) != pdPASS) {
        s_worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_has_slow && s_slow_q == NULL) {
        s_slow_q = xQueueCreate(CYPHAL_SLOW_JOBS, sizeof(job_t *));
    }
    if (s_has_slow && (s_slow_q == NULL ||
                       (s_slow == NULL && xTaskCreatePinnedToCore(slow_task, "cyphal_slow", CONFIG_CYPHAL_SLOW_STACK, NULL,
                                                                  CONFIG_CYPHAL_SLOW_PRIORITY, &s_slow,
                                                                  tskNO_AFFINITY) != pdPASS))) {
        s_slow = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_heap = multi_heap_register(s_heap_mem, CONFIG_CYPHAL_HEAP_SIZE);
    if (s_heap == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_heap_free0 = multi_heap_free_size(s_heap);
    memset(s_node, 0, sizeof(*s_node));
    const struct CanardMemoryResource mem = { .user_reference = NULL, .deallocate = heap_free, .allocate = heap_alloc };
    s_node->canard = canardInit(mem);
    s_node->txq = canardTxInit(TX_QUEUE_FRAMES, CANARD_MTU_CAN_CLASSIC, mem);
    return ESP_OK;
}

/* uavcan.node.GetInfo's response, serialized: the name as the node starts */
static esp_err_t serialize_getinfo(node_t *n)
{
    /* All zero is the type's default (no CRC, no certificate). Not _initialize_(): GCC 15 raises false
     * array-bounds errors in it (tools/gen_dsdl.py, TYPES) */
    uavcan_node_GetInfo_Response_1_0 r = { 0 };
    r.protocol_version = (uavcan_node_Version_1_0){ CANARD_CYPHAL_SPECIFICATION_VERSION_MAJOR,
                                                     CANARD_CYPHAL_SPECIFICATION_VERSION_MINOR };
    r.hardware_version = (uavcan_node_Version_1_0){ CONFIG_CYPHAL_HARDWARE_VERSION_MAJOR,
                                                     CONFIG_CYPHAL_HARDWARE_VERSION_MINOR };
    cyphal_software_version(esp_app_get_description()->version, &r.software_version.major,
                            &r.software_version.minor, &r.software_vcs_revision_id);
    memcpy(r.unique_id, s_uid, sizeof(r.unique_id));
    r.name.count = strlen(s_run.name);
    memcpy(r.name.elements, s_run.name, r.name.count);
    n->getinfo_response_size = sizeof(n->getinfo_response);
    return uavcan_node_GetInfo_Response_1_0_serialize_(&r, n->getinfo_response, &n->getinfo_response_size) < 0
               ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

/* A row's port this run: CYPHAL_PORT_OFF for an application's port that is off */
static uint16_t port_of(const service_t *svc)
{
    return svc->app_port < 0 ? svc->port_id : s_run.port_id[svc->app_port];
}

/* uavcan.node.port.List's message, serialized: s_rows[], the ports the node has while online, fixed
 * for the run. The serializer wants room for the type's largest (8.5 KB, its subject lists' extents),
 * so it and the struct (2 KB) are on the heap for the moment, not on the starting task's stack */
static esp_err_t serialize_port_list(node_t *n)
{
    const size_t room = uavcan_node_port_List_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_;
    uavcan_node_port_List_1_0 *m = psram_calloc(sizeof(*m) + room);
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t *buf = (uint8_t *)(m + 1);
    uavcan_node_port_SubjectIDList_1_0_select_sparse_list_(&m->publishers);
    uavcan_node_port_SubjectIDList_1_0_select_sparse_list_(&m->subscribers);
    for (size_t i = 0; i < s_row_count; i++) {          /* clients: none */
        const uint16_t port = port_of(&s_rows[i]);
        if (port == CYPHAL_PORT_OFF) {
            continue;
        }
        if (s_rows[i].kind == CYPHAL_PORT_SRV) {
            m->servers.mask_bitpacked_[port / 8] |= (uint8_t)(1u << (port % 8));
        } else {
            uavcan_node_port_SubjectIDList_1_0 *list = s_rows[i].kind == CYPHAL_PORT_PUB ? &m->publishers
                                                                                        : &m->subscribers;
            list->sparse_list.elements[list->sparse_list.count++].value = port;
        }
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    if (s_alloc_port_listed) {      /* the allocator answers this run: it subscribes to 8166 and publishes on it */
        m->publishers.sparse_list.elements[m->publishers.sparse_list.count++].value =
            uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_;
        m->subscribers.sparse_list.elements[m->subscribers.sparse_list.count++].value =
            uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_;
    }
#endif
    size_t size = room;
    esp_err_t err = uavcan_node_port_List_1_0_serialize_(m, buf, &size) < 0 || size > sizeof(n->port_list)
                        ? ESP_ERR_INVALID_SIZE : ESP_OK;
    if (err == ESP_OK) {
        memcpy(n->port_list, buf, size);
        n->port_list_size = size;
    }
    heap_caps_free(m);
    return err;
}

/* ------------------------------------------------------------------ the task */

/* Queue a transfer for the bus, with its deadline: whether it was queued */
static bool push(node_t *n, const struct CanardTransferMetadata *meta, const void *data, size_t size, int64_t now)
{
    /* now_usec 0: expiry is flush()'s, which drops whole transfers and counts them */
    const int32_t frames = canardTxPush(&n->txq, &n->canard, (CanardMicrosecond)(now + TX_DEADLINE_US), meta,
                                        (struct CanardPayload){ .size = size, .data = data }, 0, NULL);
    if (frames < 0) {
        s_tr_dropped++;     /* the queue is full (TX_QUEUE_FRAMES) or the heap is */
        return false;
    }
    if (n->txq.size > s_txq_peak) {
        s_txq_peak = (uint32_t)n->txq.size;
    }
    return true;
}

/* An item and the rest of its transfer, out of the queue and freed: they are all still in it,
 * behind this one, since the frames of one transfer share a CAN ID and leave in order */
static void free_transfer(node_t *n, struct CanardTxQueueItem *item)
{
    while (item != NULL) {
        struct CanardTxQueueItem *next = item->next_in_transfer;
        canardTxFree(&n->txq, &n->canard, canardTxPop(&n->txq, item));
        item = next;
    }
}

/* Everything queued, out and freed: how many transfers that was */
static uint32_t clear_tx(node_t *n)
{
    uint32_t transfers = 0;
    struct CanardTxQueueItem *item;
    for (; (item = canardTxPeek(&n->txq)) != NULL; transfers++) {
        free_transfer(n, item);
    }
    return transfers;
}

static void heartbeat(node_t *n, int64_t now)
{
    const uavcan_node_Heartbeat_1_0 hb = {
        .uptime = (uint32_t)((now - s_started_us) / 1000000),
        .health = { s_health },
        .mode = { uavcan_node_Mode_1_0_OPERATIONAL },
    };
    uint8_t buf[uavcan_node_Heartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t size = sizeof(buf);
    if (uavcan_node_Heartbeat_1_0_serialize_(&hb, buf, &size) < 0) {
        return;
    }
    const struct CanardTransferMetadata meta = {
        .priority = CanardPriorityNominal,
        .transfer_kind = CanardTransferKindMessage,
        .port_id = uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = n->heartbeat_tid++,
    };
    push(n, &meta, buf, size, now);
}

/* Plug-and-play's request (rule B). Anonymous, so a single frame, whose CAN ID libcanard ends with a
 * 7-bit discriminator made from the payload's CRC, as the spec asks: another allocatee, with another
 * pseudo-ID, has another CAN ID 127 times in 128, and the two don't collide bit for bit.
 *
 * A limit: the frame is retransmitted until acknowledged, like every other. pycyphal (and, as recalled,
 * the Cyphal/CAN spec) want anonymous frames sent without automatic retransmission, so that two
 * allocatees whose requests do share a CAN ID collide once, not in lockstep until one of them is
 * error-passive. IDF 6.1's TWAI driver can't do that for one frame: single-shot is the whole node's
 * (twai_onchip_node_config_t.fail_retry_cnt, applied to every frame in esp_twai_onchip.c's
 * _node_start_trans()), fixed until the node is deleted and made again. */
static void request(node_t *n, int64_t now)
{
    uint8_t buf[uavcan_pnp_NodeIDAllocationData_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    const size_t size = cyphal_pnp_request(buf, sizeof(buf), s_pseudo_id);
    if (size == 0) {
        return;
    }
    const struct CanardTransferMetadata meta = {
        .priority = CanardPriorityNominal,
        .transfer_kind = CanardTransferKindMessage,
        .port_id = uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = n->request_tid++,
    };
    push(n, &meta, buf, size, now);
}

static void port_list(node_t *n, int64_t now)
{
    const struct CanardTransferMetadata meta = {
        .priority = CanardPriorityOptional,     /* as the type asks */
        .transfer_kind = CanardTransferKindMessage,
        .port_id = uavcan_node_port_List_1_0_FIXED_PORT_ID_,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = n->port_list_tid++,
    };
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    size_t size = 0;
    const uint8_t *list = alloc_port_list(n, &size);
    push(n, &meta, list, size, now);
#else
    push(n, &meta, n->port_list, n->port_list_size, now);
#endif
    s_services.port_lists++;
}

/* The heartbeat, the port list and the services on n->node_id, from now: after the listen, or an
 * allocation */
static void go_online(node_t *n, int64_t now)
{
    n->canard.node_id = n->node_id;
    struct CanardRxSubscription *sub = n->subs;
    for (size_t i = 0; i < s_row_count; i++) {
        const service_t *svc = &s_rows[i];
        const uint16_t port = port_of(svc);
        if (svc->serve == NULL || port == CYPHAL_PORT_OFF) {
            continue;
        }
        canardRxSubscribe(&n->canard, svc->kind == CYPHAL_PORT_SRV ? CanardTransferKindRequest : CanardTransferKindMessage,
                          port, svc->extent, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, sub);
        sub->user_reference = (void *)svc;      /* on_frame()'s way back to the row */
        sub++;
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_online(n);
#endif
    n->next_heartbeat_us = now;
    n->next_port_list_us = now;     /* the ports have just come into use */
    s_state = CYPHAL_ONLINE;
    cyphal_wake_worker();           /* the application's poll: what it publishes can go now */
    if (s_run.node_id == CYPHAL_NODE_ID_AUTO) {     /* online with plug-and-play only by an allocation */
        ESP_LOGI(TAG, "node %u (allocated by node %u): online", n->node_id, n->allocator);
    } else {
        ESP_LOGI(TAG, "node %u (fixed): online", n->node_id);
    }
}

/* Another node has our node-ID: send nothing more, until the next start */
static void stop_in_use(node_t *n)
{
    s_state = CYPHAL_IN_USE;
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_in_use();
#endif
    s_tr_dropped += clear_tx(n);
    n->canard.node_id = CANARD_NODE_ID_UNSET;
    ESP_LOGW(TAG, "node-ID %u is in use by another node; Cyphal stopped", n->node_id);
}

/* The node-ID an allocation gave, once the listen since the start is over (cyphal_pnp_verdict()):
 * online on it, or stopped if another node has been heard with it lately (an allocator with a stale
 * or wiped table), as a collision while running would stop it (D9), but without a frame sent as that
 * node-ID. Until then nothing: still WAITING, asking no more */
static void take_up(node_t *n, int64_t now)
{
    const cyphal_pnp_verdict_t v = cyphal_pnp_verdict(&n->seen, &n->listen, n->held_id, n->held_us);
    if (v == CYPHAL_PNP_HOLD) {
        return;
    }
    n->node_id = n->held_id;
    n->held_id = CYPHAL_NODE_ID_AUTO;
    if (v == CYPHAL_PNP_TAKEN) {
        stop_in_use(n);
    } else {
        go_online(n, now);
    }
}

/* A NodeIDAllocationData received while waiting: rules C and D */
static void on_allocation(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    (void)svc;
    uint8_t id = 0;
    const cyphal_pnp_result_t r = cyphal_pnp_accept(t->payload.data, t->payload.size,
                                                    t->metadata.remote_node_id == CANARD_NODE_ID_UNSET, s_pseudo_id, &id);
    if (r == CYPHAL_PNP_INVALID) {
        return;
    }
    n->next_request_us = now + cyphal_pnp_interval_us(esp_random());     /* rule C: any allocation message */
    if (r != CYPHAL_PNP_ALLOCATED) {
        return;
    }
    /* Rule D: the request timer stops (timers() asks only while nothing is held) and the subscription
     * ends, so a later allocation, from this allocator or another, isn't heard. A request still queued is
     * pointless; one already inside canbus still goes, and an allocator answers it with the same node-ID,
     * which nobody is listening for any more */
    canardRxUnsubscribe(&n->canard, CanardTransferKindMessage, ALLOCATION.port_id);
    clear_tx(n);
    n->held_id = id;
    n->held_us = now;
    n->allocator = (uint8_t)t->metadata.remote_node_id;
    take_up(n, now);
}

/* A response's metadata: the same priority, port and transfer-ID as the request, back to whoever asked */
static struct CanardTransferMetadata response_to(const struct CanardRxTransfer *t)
{
    return (struct CanardTransferMetadata){
        .priority = t->metadata.priority,
        .transfer_kind = CanardTransferKindResponse,
        .port_id = t->metadata.port_id,
        .remote_node_id = t->metadata.remote_node_id,
        .transfer_id = t->metadata.transfer_id,
    };
}

/* A response to t, serialized into `data`, queued; size 0 (it didn't serialize) is nothing queued, and
 * counted as dropped */
static void respond(node_t *n, const struct CanardRxTransfer *t, const uint8_t *data, size_t size, int64_t now)
{
    if (size == 0) {
        s_tr_dropped++;
        return;
    }
    const struct CanardTransferMetadata meta = response_to(t);
    push(n, &meta, data, size, now);
}

static void answer_getinfo(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    (void)svc;
    respond(n, t, n->getinfo_response, n->getinfo_response_size, now);
}

/* ---- the job slots: the hand-over to the worker */

/* A free slot for the request in t, with its response's metadata and svc's worker, for the caller to put
 * the decoded request in (job->in) and hand_over(): NULL if none is free, or, for the second worker (`slow`,
 * once it exists), if it has CYPHAL_SLOW_JOBS already. Another task could take a slot the same way (taking is
 * a compare-and-swap) */
static job_t *job_for(const node_t *n, const service_t *svc, const struct CanardRxTransfer *t, bool slow)
{
    slow = slow && s_slow != NULL;
    if (slow) {
        uint32_t held = __atomic_load_n(&s_slow_held, __ATOMIC_RELAXED);
        do {
            if (held >= CYPHAL_SLOW_JOBS) {
                return NULL;
            }
        } while (!__atomic_compare_exchange_n(&s_slow_held, &held, held + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    }
    for (int i = 0; i < JOB_SLOTS; i++) {
        uint32_t expected = JOB_FREE;
        if (__atomic_compare_exchange_n(&s_job_state[i], &expected, JOB_FILLING, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            job_t *job = &s_jobs[i];
            job->run = n->run;
            job->slow = slow;
            job->work = svc->work;
            job->app = svc->app;
            job->meta = response_to(t);
            job->size = 0;
            return job;
        }
    }
    if (slow) {
        __atomic_sub_fetch(&s_slow_held, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* Released: what was written into the slot before is seen by whoever acquires the new state */
static void job_set(const job_t *job, uint32_t state)
{
    __atomic_store_n(&s_job_state[job - s_jobs], state, __ATOMIC_RELEASE);
}

/* A slot taken by job_for() and not handed over, free again */
static void job_drop(job_t *job)
{
    if (job->slow) {
        __atomic_sub_fetch(&s_slow_held, 1, __ATOMIC_RELEASE);
    }
    job_set(job, JOB_FREE);
}

/* A filled slot to its worker: false, and the slot free again, if it couldn't be queued */
static bool hand_over(job_t *job)
{
    job_set(job, JOB_WORK);
    /* Each queue is as long as the jobs that may be in it (JOB_SLOTS, CYPHAL_SLOW_JOBS): never full, but never block */
    if (xQueueSend(job->slow ? s_slow_q : s_work_q, &job, 0) != pdTRUE) {
        job_drop(job);
        return false;
    }
    return true;
}

/* Whether `a` comes before `b` in a mailbox's commit order, which wraps */
static bool before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* A READY buffer leaves that state: the counts, under m->mux */
static void unready_locked(mailbox_t *m)
{
    __atomic_store_n(&m->ready, m->ready - 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&s_mail_ready, 1, __ATOMIC_RELAXED);
}

/* The mailboxes' messages into the transmit queue, oldest first, each at its port's priority with the port's next
 * transfer-ID, straight from its buffer (push() copies it), which is then freed. Only online, on a port that isn't
 * off this run (cyphal_publish() takes nothing otherwise, but a message can be committed just before the node
 * stops: it is dropped). A mailbox with nothing waiting is passed over without its lock. True if a message was held
 * back for want of room in the queue: it still counts in mail_waiting(), which node_task() must then not read as new
 * mail (it would never sleep while the queue stays full, as it does bus-off) */
static bool collect_mail(node_t *n, int64_t now)
{
    bool held = false;
    if (__atomic_load_n(&s_mail_ready, __ATOMIC_ACQUIRE) == 0) {
        return false;
    }
    for (size_t i = 0; i < s_mail_count; i++) {
        mailbox_t *m = &s_mail[i];
        while (__atomic_load_n(&m->ready, __ATOMIC_ACQUIRE) != 0) {
            int b = -1;
            const size_t room = n->txq.capacity - n->txq.size;
            taskENTER_CRITICAL(&m->mux);
            for (int k = 0; k <= m->depth; k++) {
                if (m->state[k] == BUF_READY && (b < 0 || before(m->order[k], m->order[b]))) {
                    b = k;
                }
            }
            /* Not unless the queue has room for all of it: it waits for a later turn instead (one bigger than the
             * whole queue never would, so it goes, for libcanard to refuse and count) */
            const size_t frames = b < 0 ? 0 : cyphal_frames(m->size[b]);
            if (b >= 0 && frames > room && frames <= n->txq.capacity) {
                b = -1;
                held = true;
            }
            if (b >= 0) {
                m->state[b] = BUF_SENDING;
                unready_locked(m);
            }
            taskEXIT_CRITICAL(&m->mux);
            if (b < 0) {
                break;
            }
            const uint16_t port = s_run.port_id[m->port];
            bool queued = false;
            if (s_state == CYPHAL_ONLINE && port != CYPHAL_PORT_OFF) {
                const struct CanardTransferMetadata meta = {
                    .priority = (enum CanardPriority)cyphal_port(m->port)->priority,     /* the same numbers, 0 the most urgent */
                    .transfer_kind = CanardTransferKindMessage,
                    .port_id = port,
                    .remote_node_id = CANARD_NODE_ID_UNSET,
                    .transfer_id = m->tid++,
                };
                queued = push(n, &meta, m->data + (size_t)b * m->max, m->size[b], now);  /* counts its failure */
            } else {
                s_tr_dropped++;         /* committed just before the node stopped, or its port is off this run */
            }
            taskENTER_CRITICAL(&m->mux);
            m->state[b] = BUF_FREE;
            m->published += queued;
            taskEXIT_CRITICAL(&m->mux);
        }
    }
    return held;
}

/* Whether a mailbox holds a message collect_mail() hasn't taken yet: no lock taken */
static bool mail_waiting(void)
{
    return __atomic_load_n(&s_mail_ready, __ATOMIC_ACQUIRE) != 0;
}

/* The worker's finished jobs, their responses into the transmit queue (sent by flush(), after the
 * receive queue has been read like everything else), and their slots freed. A job from an earlier start,
 * or one done after the node stopped over its node-ID, is dropped; a response that didn't serialize is
 * counted as dropped. Then the mailboxes: true if one held a message back (collect_mail()) */
static bool collect(node_t *n)
{
    for (int i = 0; i < JOB_SLOTS; i++) {
        if (__atomic_load_n(&s_job_state[i], __ATOMIC_ACQUIRE) != JOB_DONE) {
            continue;
        }
        const job_t *job = &s_jobs[i];
        if (job->run == n->run && s_state == CYPHAL_ONLINE) {
            if (job->size > 0) {
                push(n, &job->meta, job->data, job->size, esp_timer_get_time());
            } else {
                s_tr_dropped++;
            }
        }
        job_set(job, JOB_FREE);
    }
    return collect_mail(n, esp_timer_get_time());
}

/* Whether the worker has finished a job collect() hasn't taken yet */
static bool jobs_done(void)
{
    for (int i = 0; i < JOB_SLOTS; i++) {
        if (__atomic_load_n(&s_job_state[i], __ATOMIC_ACQUIRE) == JOB_DONE) {
            return true;
        }
    }
    return false;
}

/* ---- uavcan.register.Access and List */

/* An Access response for register `index` (-1 for none) whose value is in r->value already: its flags,
 * serialized into `out` (room bytes). Its size, 0 if it didn't serialize */
static size_t access_response(uavcan_register_Access_Response_1_0 *r, int index, uint8_t *out, size_t room)
{
    const cyphal_register_t *reg = index >= 0 ? cyphal_reg((size_t)index) : NULL;
    r->_mutable = reg != NULL && (reg->flags & CYPHAL_REG_MUTABLE);
    r->persistent = reg != NULL && (reg->flags & CYPHAL_REG_PERSISTENT);
    if (reg == NULL) {
        uavcan_register_Value_1_0_select_empty_(&r->value);     /* no such register; flags cleared */
    }
    return uavcan_register_Access_Response_1_0_serialize_(r, out, &room) < 0 ? 0 : room;
}

/* A read is answered here (cyphal_register_t's read never waits), but for a CYPHAL_REG_READ_ON_WORKER register's,
 * which goes to the worker; a write to a register there is goes to the worker (NVS). With no slot free, a write is
 * answered here with the register as it is, unwritten, which tells the caller it wasn't done, and a read on the
 * worker isn't answered at all (Access has no way to say busy): the client times out and asks again. Both count as
 * busy */
static void on_access(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    uavcan_register_Access_Request_1_0 *q = &n->rq.access;
    size_t size = t->payload.size;
    if (uavcan_register_Access_Request_1_0_deserialize_(q, t->payload.data, &size) < 0) {
        return;
    }
    const int index = cyphal_reg_find(q->name.name.elements, q->name.name.count);
    const cyphal_register_t *reg = index >= 0 ? cyphal_reg((size_t)index) : NULL;
    const bool write = reg != NULL && !uavcan_register_Value_1_0_is_empty_(&q->value);
    if (write || (reg != NULL && (reg->flags & CYPHAL_REG_READ_ON_WORKER))) {
        job_t *job = job_for(n, svc, t, false);
        if (job != NULL) {
            job->in.write.index = (size_t)index;
            job->in.write.value = q->value;     /* empty for a read */
            if (hand_over(job)) {
                return;
            }
        }
        s_services.busy++;
        if (!write) {
            return;
        }
    } else {
        s_services.register_reads++;
    }
    uavcan_register_Access_Response_1_0 *r = &n->rs.access;
    memset(r, 0, sizeof(*r));
    if (index >= 0) {
        cyphal_settings_t s;
        saved_copy(&s);
        cyphal_reg_read(&s, (size_t)index, &r->value);
    }
    respond(n, t, n->out, access_response(r, index, n->out, OUT_BYTES), now);
}

static void on_list(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    (void)svc;
    uavcan_register_List_Request_1_0 q = { 0 };
    size_t size = t->payload.size;
    if (uavcan_register_List_Request_1_0_deserialize_(&q, t->payload.data, &size) < 0) {
        return;
    }
    s_services.register_lists++;
    uavcan_register_List_Response_1_0 *r = &n->rs.list;
    memset(r, 0, sizeof(*r));
    const char *name = cyphal_reg_name(q.index);    /* past the last: an empty name, the end of the list */
    if (name != NULL) {
        r->name.name.count = strlen(name);
        memcpy(r->name.name.elements, name, r->name.name.count);
    }
    size = OUT_BYTES;
    respond(n, t, n->out, uavcan_register_List_Response_1_0_serialize_(r, n->out, &size) < 0 ? 0 : size, now);
}

/* ---- uavcan.node.ExecuteCommand */

static size_t command_response(uavcan_node_ExecuteCommand_Response_1_3 *r, uint8_t status, uint8_t *out, size_t room)
{
    memset(r, 0, sizeof(*r));
    r->status = status;     /* and no output: the standard commands have none */
    return uavcan_node_ExecuteCommand_Response_1_3_serialize_(r, out, &room) < 0 ? 0 : room;
}

/* Restart, factory reset and identify go to the worker, if the application has given the node the means
 * (cyphal_app_t); storing is done already (every register is saved as it is written); anything else is
 * a bad command. With no slot free, failure: the caller tries again */
static void on_command(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    uavcan_node_ExecuteCommand_Request_1_3 *q = &n->rq.command;
    size_t size = t->payload.size;
    if (uavcan_node_ExecuteCommand_Request_1_3_deserialize_(q, t->payload.data, &size) < 0) {
        return;
    }
    s_services.commands++;
    uint8_t status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_BAD_COMMAND;
    const uint16_t c = q->command;
    if (c == uavcan_node_ExecuteCommand_Request_1_3_COMMAND_STORE_PERSISTENT_STATES) {
        status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_SUCCESS;
    } else if (((c == uavcan_node_ExecuteCommand_Request_1_3_COMMAND_RESTART ||
                 c == uavcan_node_ExecuteCommand_Request_1_3_COMMAND_FACTORY_RESET) && s_app.restart != NULL) ||
               (c == uavcan_node_ExecuteCommand_Request_1_3_COMMAND_IDENTIFY && s_app.identify != NULL)) {
        /* Identify on the second worker when the application says it can take long (cyphal_app_t) */
        job_t *job = job_for(n, svc, t, c == uavcan_node_ExecuteCommand_Request_1_3_COMMAND_IDENTIFY &&
                                        s_app.identify_slow);
        if (job != NULL) {
            job->in.command = *q;
            if (hand_over(job)) {
                return;
            }
        }
        s_services.busy++;
        status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_FAILURE;
    } else {
        s_services.commands_refused++;
    }
    respond(n, t, n->out, command_response(&n->rs.command, status, n->out, OUT_BYTES), now);
}

/* ---- the application's ports */

/* A request to one of the application's services: decoded into a free job slot by its handler, and handed to
 * the worker; with none free, its handler's busy answer at once. One that doesn't decode isn't answered */
static void on_app_request(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    const cyphal_handler_t *h = svc->app;
    job_t *job = job_for(n, svc, t, h->slow);
    if (job != NULL) {
        memset(job->in.app, 0, h->in_size);
        if (!h->decode(t->payload.data, t->payload.size, job->in.app)) {
            job_drop(job);
            return;
        }
        s_services.app_requests++;
        if (hand_over(job)) {
            return;
        }
    }
    s_services.busy++;
    const size_t size = h->busy(n->out, OUT_BYTES);
    if (size > 0) {         /* 0: a type with no way to say busy, so no answer at all, and the client asks again */
        respond(n, t, n->out, size, now);
    }
}

/* A message on one of the application's subscriptions: its handler's, at once */
static void on_app_message(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    (void)n;
    (void)now;
    s_services.app_messages++;
    svc->app->on_message(t->payload.data, t->payload.size, (uint8_t)t->metadata.remote_node_id);
}

/* An application's request, by its handler, on the worker */
static bool work_app(job_t *job)
{
    job->size = job->app->work(job->in.app, job->meta.remote_node_id, job->data, JOB_BYTES);
    return false;
}

esp_err_t cyphal_publish(size_t port, const void *data, size_t size)
{
    mailbox_t *m = NULL;
    for (size_t i = 0; i < s_mail_count && m == NULL; i++) {
        m = s_mail[i].port == port ? &s_mail[i] : NULL;
    }
    if (m == NULL || s_run.port_id[port] == CYPHAL_PORT_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (size == 0 || size > m->max) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_state != CYPHAL_ONLINE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A free buffer while fewer than its depth wait (those being filled count); full, depth 1 takes the oldest
     * waiting one instead, which this replaces, and a queue refuses */
    int b = -1, oldest = -1;
    unsigned waiting = 0;
    taskENTER_CRITICAL(&m->mux);
    for (int k = 0; k <= m->depth; k++) {
        if (m->state[k] == BUF_FREE && b < 0) {
            b = k;
        } else if (m->state[k] == BUF_READY || m->state[k] == BUF_FILLING) {
            waiting++;
            if (m->state[k] == BUF_READY && (oldest < 0 || before(m->order[k], m->order[oldest]))) {
                oldest = k;
            }
        }
    }
    if (waiting >= m->depth) {
        b = m->depth == 1 ? oldest : -1;
        if (b >= 0) {
            unready_locked(m);
            m->replaced++;
        } else {
            m->refused++;
        }
    }
    if (b >= 0) {
        m->state[b] = BUF_FILLING;
    }
    taskEXIT_CRITICAL(&m->mux);
    if (b < 0) {
        return ESP_ERR_NOT_FINISHED;
    }
    memcpy(m->data + (size_t)b * m->max, data, size);
    taskENTER_CRITICAL(&m->mux);
    m->size[b] = size;
    m->order[b] = m->next_order++;
    m->state[b] = BUF_READY;
    __atomic_store_n(&m->ready, m->ready + 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_mail_ready, 1, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&m->mux);
    canbus_wake(__atomic_load_n(&s_bus, __ATOMIC_RELAXED));     /* refused if the node has stopped: nobody waits */
    return ESP_OK;
}

void cyphal_wake_worker(void)
{
    if (s_work_q == NULL) {
        return;
    }
    uint32_t idle = 0;
    if (__atomic_compare_exchange_n(&s_wake_queued, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        job_t *const wake = NULL;
        if (xQueueSend(s_work_q, &wake, 0) != pdTRUE) {     /* a place of its own: never, but never block */
            __atomic_store_n(&s_wake_queued, 0, __ATOMIC_RELEASE);
        }
    }
}

void cyphal_set_health(uint8_t health)
{
    s_health = health > CYPHAL_HEALTH_WARNING ? CYPHAL_HEALTH_WARNING : health;
}

/* ---- the worker */

/* A register written, by the worker, then answered with what it holds now; or a CYPHAL_REG_READ_ON_WORKER register
 * read (an empty value) */
static bool work_access(job_t *job)
{
    uavcan_register_Access_Response_1_0 *r = &s_ws->access;
    memset(r, 0, sizeof(*r));
    const size_t index = job->in.write.index;
    const cyphal_register_t *reg = cyphal_reg(index);
    if (uavcan_register_Value_1_0_is_empty_(&job->in.write.value)) {
        cyphal_settings_t s;
        saved_copy(&s);
        cyphal_reg_read(&s, index, &r->value);
        s_services.register_reads++;
        job->size = access_response(r, (int)index, job->data, JOB_BYTES);
        return false;
    }
    char why[128];
    const esp_err_t err = cyphal_write_register(index, &job->in.write.value, &r->value, why, sizeof(why));
    if (err == ESP_OK) {
        s_services.register_writes++;
#if CONFIG_CYPHAL_PNP_ALLOCATOR
        ESP_LOGI(TAG, "%s written by node %u%s", reg->name, job->meta.remote_node_id,
                 (reg->flags & CYPHAL_REG_NEXT_START) || alloc_is_register(reg) ? ", from the node's next start" : "");
#else
        ESP_LOGI(TAG, "%s written by node %u%s", reg->name, job->meta.remote_node_id,
                 reg->flags & CYPHAL_REG_NEXT_START ? ", from the node's next start" : "");
#endif
    } else {
        s_services.register_refused++;
        ESP_LOGW(TAG, "%s: node %u's write refused: %s", reg->name, job->meta.remote_node_id,
                 err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED ? why : esp_err_to_name(err));
    }
    job->size = access_response(r, (int)index, job->data, JOB_BYTES);
    return false;
}

static bool work_command(job_t *job)
{
    uint8_t status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_INTERNAL_ERROR;
    bool restart = false;
    const unsigned from = job->meta.remote_node_id;
    esp_err_t err;
    switch (job->in.command.command) {
    case uavcan_node_ExecuteCommand_Request_1_3_COMMAND_RESTART:
        ESP_LOGW(TAG, "node %u asks for a restart: restarting in %d ms", from, RESTART_AFTER_MS);
        status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_SUCCESS;
        restart = true;
        break;
    case uavcan_node_ExecuteCommand_Request_1_3_COMMAND_FACTORY_RESET:
        /* The node's settings, every register, then the application's own: both, or neither */
        err = factory_reset();
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "node %u asks for a factory reset: done, restarting in %d ms", from, RESTART_AFTER_MS);
            status = uavcan_node_ExecuteCommand_Response_1_3_STATUS_SUCCESS;
            restart = true;
        } else {
            ESP_LOGE(TAG, "node %u asks for a factory reset: %s; nothing reset", from, esp_err_to_name(err));
        }
        break;
    case uavcan_node_ExecuteCommand_Request_1_3_COMMAND_IDENTIFY:
        err = s_app.identify();
        ESP_LOGI(TAG, "node %u asks the board to identify itself%s%s", from, err == ESP_OK ? "" : ": ",
                 err == ESP_OK ? "" : esp_err_to_name(err));
        status = err == ESP_OK ? uavcan_node_ExecuteCommand_Response_1_3_STATUS_SUCCESS
                               : uavcan_node_ExecuteCommand_Response_1_3_STATUS_FAILURE;
        break;
    default:    /* on_command() hands over nothing else */
        break;
    }
    /* On its worker's stack, not s_ws: identify runs on the second worker (identify_slow) while the worker may be
     * building an Access response there. About 50 bytes */
    uavcan_node_ExecuteCommand_Response_1_3 r;
    job->size = command_response(&r, status, job->data, JOB_BYTES);
    return restart;
}

/* A job's work, then the node's task woken for its response. A restart is asked for after the wake, and waits
 * RESTART_AFTER_MS, the response's deadline: by then it has gone, or been dropped */
static void run_job(job_t *job, const char *who, int stack)
{
    const bool restart = job->work(job);
    if (job->slow) {
        __atomic_sub_fetch(&s_slow_held, 1, __ATOMIC_RELEASE);
    }
    job_set(job, JOB_DONE);     /* the node's task's from here: not touched again */
    canbus_wake(__atomic_load_n(&s_bus, __ATOMIC_RELAXED));     /* refused if the node has stopped: nobody waits */
    if (restart) {
        /* The deepest the worker goes (a factory reset's NVS writes), which `cyphal` never sees: the
         * board restarts before anyone could look */
        ESP_LOGI(TAG, "%s: its stack %u of %d bytes never used, before the restart", who,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), stack);
        s_app.restart(RESTART_AFTER_MS);
    }
}

/* The second worker's jobs (cyphal_handler_t.slow, cyphal_app_t.identify_slow), one at a time, in the order handed
 * over */
static void slow_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job = NULL;
        if (xQueueReceive(s_slow_q, &job, portMAX_DELAY) == pdTRUE && job != NULL) {
            run_job(job, "second worker", CONFIG_CYPHAL_SLOW_STACK);
        }
    }
}

/* The jobs, one at a time, in the order handed over; the node's task woken for each response (run_job()).
 * Between jobs, the application's poll, when it is due or a wake asks for it */
static void worker_task(void *arg)
{
    (void)arg;
    int64_t next_poll_us = 0;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_app.poll != NULL) {
            const int64_t left_us = next_poll_us - esp_timer_get_time();
            const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
            wait = left_us <= 0 ? 0 : (TickType_t)((left_us + tick_us - 1) / tick_us);
        }
        job_t *job = NULL;
        const bool got = xQueueReceive(s_work_q, &job, wait) == pdTRUE;
        if (got && job == NULL) {
            __atomic_store_n(&s_wake_queued, 0, __ATOMIC_RELEASE);     /* a wake after this queues another */
        }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
        alloc_worker();     /* a row the node's task added (it wakes the worker for it) */
#endif
        if (s_app.poll != NULL && ((got && job == NULL) || esp_timer_get_time() >= next_poll_us)) {
            const uint32_t ms = s_app.poll(s_state == CYPHAL_ONLINE);
            next_poll_us = esp_timer_get_time() + (int64_t)ms * 1000;
        }
        if (job != NULL) {
            run_job(job, "worker", CONFIG_CYPHAL_WORKER_STACK);
        }
    }
}

static void on_frame(node_t *n, const canbus_frame_t *f)
{
    const int64_t stamp = (int64_t)f->timestamp_us;
    cyphal_listen_frame(&n->listen, stamp);
    const cyphal_state_t state = s_state;
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_frame(n, f, state, stamp);
#endif
    if (state == CYPHAL_WAITING) {
        /* For an allocation's check, made only while waiting (an allocation held included); waiting
         * comes only at a start, which clears the table, so no frame from before it matters */
        cyphal_seen_frame(&n->seen, cyphal_frame_source(f->id, f->ext, f->rtr, f->len), stamp);
    } else if ((state == CYPHAL_LISTENING || state == CYPHAL_ONLINE) &&
               cyphal_frame_source(f->id, f->ext, f->rtr, f->len) == n->node_id) {
        /* The controller never receives its own frames, so this is another node's (D9) */
        stop_in_use(n);
        return;
    }
    if ((state != CYPHAL_ONLINE && state != CYPHAL_WAITING) || !f->ext || f->rtr) {
        return;
    }
    const struct CanardFrame frame = {
        .extended_can_id = f->id,
        .payload = { .size = f->len, .data = f->data },
    };
    struct CanardRxTransfer t;
    struct CanardRxSubscription *sub = NULL;
    if (canardRxAccept(&n->canard, (CanardMicrosecond)f->timestamp_us, &frame, 0, &t, &sub) != 1) {
        return;
    }
    s_tr_received++;
    const service_t *svc = sub->user_reference;     /* every subscription's row: go_online(), start_locked() */
    svc->serve(n, svc, &t, stamp);                  /* a response's deadline from the request's arrival */
    heap_free(NULL, t.payload.allocated_size, t.payload.data);     /* the instance's memory resource */
}

/* Bus-off: the node brings the controller back itself (canbus counts how often), backing off when
 * bus-offs come back to back (cyphal_busoff_seen()). Asked again at each look while still bus-off:
 * a recovery needs 128 idle periods of 11 bits, which a busy bus can hold off. */
static void check_bus(node_t *n, int64_t now)
{
    canbus_status_t st;
    canbus_status(&st);
    if (st.state != CANBUS_BUS_OFF) {
        s_recover_at_ms = 0;
        return;
    }
    if (st.bus_off != n->bus_off_seen) {
        n->bus_off_seen = st.bus_off;
        bool log;
        n->recover_at_us = cyphal_busoff_seen(&n->busoff, now, &log);
        const int64_t wait_us = n->recover_at_us - now;
        if (log && n->busoff.run == 1) {
            ESP_LOGW(TAG, "bus-off (%lu times since the start): recovering", (unsigned long)st.bus_off);
        } else if (log) {
            ESP_LOGW(TAG, "bus-off again, %lu times in a row (%lu since the start): recovering in %lld s; "
                     "logged again at %lu in a row", (unsigned long)n->busoff.run, (unsigned long)st.bus_off,
                     (long long)(wait_us / 1000000), (unsigned long)n->busoff.run * 2);
        }
    }
    if (now >= n->recover_at_us) {
        s_recover_at_ms = 0;
        if (canbus_recover(s_bus) == ESP_OK) {
            cyphal_busoff_recovering(&n->busoff, now);
        }
    } else {
        const uint32_t at_ms = (uint32_t)(n->recover_at_us / 1000);
        s_recover_at_ms = at_ms != 0 ? at_ms : 1;
    }
}

/* Every frame waiting in the receive queue, through on_frame(). True when it was emptied; false
 * when RX_BUDGET frames were read and more may wait, or the node is stopping. */
static bool drain(node_t *n)
{
    canbus_frame_t f;
    for (int budget = RX_BUDGET; budget > 0 && s_running; budget--) {
        const int64_t t = esp_timer_get_time();
        if (!canbus_receive(s_bus, &f, 0)) {
            cyphal_listen_empty(&n->listen, t);
            return true;
        }
        on_frame(n, &f);
    }
    return false;
}

/* The transmit queue's first frame, once every transfer whose deadline has passed is dropped, wherever
 * it is in the priority order (canbus can't take a frame back, so the deadline is applied before it
 * gets one). By libcanard's deadline tree, whose leftmost frame is the first to expire: equal deadlines
 * are in the order pushed, so it is always the first frame left of its transfer, as free_transfer()
 * wants. The head of the priority queue alone isn't enough: a heartbeat always waiting, as one is
 * while nobody acknowledges, would keep a port list (lower priority) behind it from ever expiring.
 *
 * This leans on libcanard v4.0.0's insides, the pinned version: CanardTxQueue.deadline_root and
 * CanardTxQueueItem.deadline_base (canard.h lets lr be read, but the tree's order and the item's layout
 * are its own), and it walks the tree as v4.0.0's private expiry does (canard.c, txFlushExpiredTransfers()),
 * so that it drops whole transfers and counts each once in "dropped"; the public expiry (now_usec given
 * to canardTxPush() or canardTxPoll()) counts frames, which would change what "dropped" means. On a bump, read both again: the
 * fields, the tree's comparator (ties in push order) and how the private walk frees a transfer. */
static struct CanardTxQueueItem *next_due(node_t *n)
{
    const int64_t now = esp_timer_get_time();
    for (struct CanardTreeNode *t = n->txq.deadline_root; t != NULL; t = n->txq.deadline_root) {
        while (t->lr[0] != NULL) {      /* canard.h: lr may be read for traversal */
            t = t->lr[0];
        }
        struct CanardTxQueueItem *item =
            (struct CanardTxQueueItem *)((char *)t - offsetof(struct CanardTxQueueItem, deadline_base));
        if ((int64_t)item->tx_deadline_usec >= now) {
            break;
        }
        free_transfer(n, item);
        s_tr_dropped++;
    }
    return canardTxPeek(&n->txq);
}

/* Hand libcanard's queue to canbus, highest priority first, one frame at a time: room in canbus
 * first (the frame before on the bus; waiting up to a tick for that), then every frame received
 * meanwhile through on_frame(), and only then the next frame. So once a frame from our node-ID
 * is in the receive queue, nothing more goes but the frame already inside canbus. Expired
 * transfers are dropped whether or not there is room, so that a bus with nobody acknowledging
 * can't fill the queue. True when the queue is empty; false when the rest must wait: no room yet
 * (nobody acknowledging), frames still to read, or the controller bus-off, in which case *retry
 * says to wait a tick before trying again. */
static bool flush(node_t *n, bool *retry)
{
    *retry = false;
    while (next_due(n) != NULL) {
        esp_err_t err = canbus_wait_room(s_bus, 1);
        if (err != ESP_OK) {
            *retry = err != ESP_ERR_TIMEOUT;    /* a timeout has waited its tick already */
            return false;
        }
        if (!drain(n)) {
            return false;
        }
        /* Again: time has passed, and a frame just read may have stopped the node (the queue
         * cleared) or queued an answer ahead of what was first */
        struct CanardTxQueueItem *const item = next_due(n);
        if (item == NULL) {
            break;
        }
        canbus_frame_t f = {
            .id = item->frame.extended_can_id,
            .ext = true,
            .len = (uint8_t)item->frame.payload.size,
        };
        memcpy(f.data, item->frame.payload.data, f.len);
        err = canbus_send(s_bus, &f, 0);
        if (err != ESP_OK) {
            *retry = err != ESP_ERR_TIMEOUT;
            return false;
        }
        s_frames_queued++;
        if (f.len > 0 && (f.data[f.len - 1] & 0x40)) {     /* the tail byte's end of transfer */
            s_tr_sent++;
        }
        canardTxFree(&n->txq, &n->canard, canardTxPop(&n->txq, item));
    }
    return true;
}

static void timers(node_t *n, int64_t now)
{
    if (s_state == CYPHAL_LISTENING && cyphal_listen_over(&n->listen)) {
        go_online(n, now);
    }
    if (s_state == CYPHAL_WAITING && n->held_id != CYPHAL_NODE_ID_AUTO) {
        take_up(n, now);
    } else if (s_state == CYPHAL_WAITING && now >= n->next_request_us) {
        n->next_request_us = now + cyphal_pnp_interval_us(esp_random());     /* rule B */
        /* Not while the last is still queued (requests are all a waiting node sends): nobody has
         * acknowledged the one inside canbus, and another would only expire behind it */
        if (canardTxPeek(&n->txq) == NULL) {
            request(n, now);
        }
    }
    if (s_state == CYPHAL_ONLINE && now >= n->next_heartbeat_us) {
        heartbeat(n, now);
        n->next_heartbeat_us += HEARTBEAT_US;
        if (n->next_heartbeat_us <= now) {     /* late by a period or more: no catching up in a burst */
            n->next_heartbeat_us = now + HEARTBEAT_US;
        }
    }
    if (s_state == CYPHAL_ONLINE && now >= n->next_port_list_us) {
        port_list(n, now);
        /* By the schedule, as the heartbeat is, so that a late turn doesn't push the next past its
         * period, the type's maximum */
        n->next_port_list_us += PORT_LIST_US;
        if (n->next_port_list_us <= now) {
            n->next_port_list_us = now + PORT_LIST_US;
        }
    }
    if (now >= n->next_bus_check_us) {
        n->next_bus_check_us = now + BUS_CHECK_US;
        check_bus(n, now);
    }
}

/* Ticks to the next timer, rounded up: a second at most, the bus check's period */
static TickType_t ticks_to_next(const node_t *n, int64_t now)
{
    int64_t next = n->next_bus_check_us;
    const bool held = s_state == CYPHAL_WAITING && n->held_id != CYPHAL_NODE_ID_AUTO;
    if ((s_state == CYPHAL_LISTENING || held) && cyphal_listen_wake_us(&n->listen) < next) {
        next = cyphal_listen_wake_us(&n->listen);
    }
    if (s_state == CYPHAL_WAITING && !held && n->next_request_us < next) {
        next = n->next_request_us;
    }
    if (s_state == CYPHAL_ONLINE && n->next_heartbeat_us < next) {
        next = n->next_heartbeat_us;
    }
    if (s_state == CYPHAL_ONLINE && n->next_port_list_us < next) {
        next = n->next_port_list_us;
    }
    const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
    return next <= now ? 0 : (TickType_t)((next - now + tick_us - 1) / tick_us);
}

static void node_task(void *arg)
{
    node_t *n = arg;
    while (s_running) {
        /* What has come first (D9), then the timers and the worker's responses, then the transmit
         * queue: all of them only queue, and only flush() sends */
        const bool drained = drain(n);
        timers(n, esp_timer_get_time());
        const bool held = collect(n);
        bool retry = false;
        const bool sent = drained && (canardTxPeek(&n->txq) == NULL || flush(n, &retry));
        /* Frames still to read, or to send; else the next timer, as of after flush(): a frame it read
         * may have changed the state (an allocation taken up makes a heartbeat due at once). Not at all
         * if the worker has finished a job since collect(): its wake may have been read and skipped by
         * flush()'s drain(), and the response would wait for the next timer, up to a second. Round the
         * loop instead, where it is collected and sent after the receive queue is read, as always. The same
         * for a message put in a mailbox: its wake may have been read the same way.
         * A message collect() held back for room is no new mail: with the queue still full it waits as the queue
         * does, a tick when flush() asks for one (bus-off: canbus refuses at once, and a wait of 0 here would spin
         * this task, the highest on its core, until frames expire); once flush() has emptied the queue, round the
         * loop at once to queue it. A message that came in meanwhile waits a tick at most: the queue isn't empty */
        const bool mail = held ? sent : mail_waiting();
        const TickType_t wait = jobs_done() || mail ? 0 : !sent ? (retry ? 1 : 0)
                                                               : ticks_to_next(n, esp_timer_get_time());
        /* Asleep until the next timer or frame, or a canbus_wake(): the worker's, or a stop's. Looked at
         * once more just before: a stop's wake read and skipped by a receive above (drain()'s) came after
         * s_running was cleared, so this sees it; one not read yet ends the sleep below at once */
        if (!s_running) {
            break;
        }
        canbus_frame_t f;
        if (canbus_receive(s_bus, &f, wait) && s_running) {
            on_frame(n, &f);
        }
    }
    xSemaphoreGive(s_task_ended);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ start and stop */

static esp_err_t start_locked(int tx, int rx)
{
    if (s_task != NULL || s_bus != 0) {     /* running, or the controller kept by a failed stop */
        return ESP_ERR_INVALID_STATE;
    }
    s_tx = tx;
    s_rx = rx;
    s_run = s_saved;
    s_tr_sent = s_tr_received = s_tr_dropped = s_frames_queued = s_heap_failed = s_txq_peak = 0;
    s_services = (cyphal_services_t){ 0 };
    for (size_t i = 0; i < s_mail_count; i++) {
        /* What waits from the last run is dropped; a buffer a publisher is filling is its to commit */
        mailbox_t *m = &s_mail[i];
        taskENTER_CRITICAL(&m->mux);
        for (int k = 0; k <= m->depth; k++) {
            if (m->state[k] == BUF_READY) {
                m->state[k] = BUF_FREE;
                unready_locked(m);
            }
        }
        m->published = m->replaced = m->refused = 0;
        taskEXIT_CRITICAL(&m->mux);
        m->tid = 0;
    }
    s_error = ESP_OK;
    s_stop_failed = false;
    esp_err_t err = make_node();
    node_t *n = s_node;
    if (err == ESP_OK) {
        n->run = ++s_runs;      /* a job of an earlier start, still with the worker, is dropped */
        err = serialize_getinfo(n);
    }
    if (err == ESP_OK) {
        err = serialize_port_list(n);
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    if (err == ESP_OK) {
        err = alloc_port_lists(n);
    }
#endif
    if (err == ESP_OK) {
        const canbus_config_t c = {
            .tx_gpio = tx,
            .rx_gpio = rx,
            .bitrate = s_run.bitrate,
            .mode = CANBUS_NORMAL,     /* acknowledging the bus even while the node sends nothing */
            .owner = "the Cyphal node",
            .tx_slots = 1,              /* one frame at a time: libcanard's queue keeps the order */
        };
        err = canbus_start(&c, &s_bus);
    }
    if (err != ESP_OK) {
        s_state = CYPHAL_FAILED;
        s_error = err;
        ESP_LOGW(TAG, "not started on GPIO%d and GPIO%d: %s", tx, rx,
                 err == ESP_ERR_INVALID_STATE ? "the controller is in use" : esp_err_to_name(err));
        return err;
    }
    const int64_t now = esp_timer_get_time();
    s_started_us = now;
    n->node_id = s_run.node_id;
    n->held_id = CYPHAL_NODE_ID_AUTO;
    cyphal_seen_clear(&n->seen);
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_start();
#endif
    n->next_bus_check_us = now + BUS_CHECK_US;
    cyphal_busoff_init(&n->busoff);
    s_recover_at_ms = 0;
    /* Logged now, before the task can log what happens next. By the setting, not by s_state, which
     * the task (at a higher priority) may change as soon as it exists */
    if (s_run.node_id == CYPHAL_NODE_ID_AUTO) {
        cyphal_listen_start(&n->listen, now, CYPHAL_PNP_LISTEN_US);
        /* Rule A: subscribed, and the first request after a random Trequest, not at once */
        canardRxSubscribe(&n->canard, CanardTransferKindMessage, ALLOCATION.port_id, ALLOCATION.extent,
                          CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, &n->allocation);
        n->allocation.user_reference = (void *)&ALLOCATION;
        n->next_request_us = now + cyphal_pnp_interval_us(esp_random());
        s_state = CYPHAL_WAITING;
        ESP_LOGI(TAG, "plug-and-play, %s, on GPIO%d (TX) and GPIO%d (RX): asking for a node-ID, pseudo-ID %012llx",
                 s_run.name, tx, rx, (unsigned long long)s_pseudo_id);
    } else {
        cyphal_listen_start(&n->listen, now, (int64_t)CYPHAL_LISTEN_MS * 1000);
        s_state = CYPHAL_LISTENING;
        ESP_LOGI(TAG, "node %u (fixed), %s, on GPIO%d (TX) and GPIO%d (RX): listening for %d s first", n->node_id,
                 s_run.name, tx, rx, CYPHAL_LISTEN_MS / 1000);
    }
    s_running = true;
    if (xTaskCreatePinnedToCore(node_task, "cyphal", CONFIG_CYPHAL_TASK_STACK, n, CONFIG_CYPHAL_TASK_PRIORITY,
                                &s_task, CONFIG_CYPHAL_TASK_CORE) != pdPASS) {
        s_running = false;
        s_task = NULL;
        canbus_stop(s_bus);
        s_bus = 0;
        s_state = CYPHAL_FAILED;
        s_error = ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "not started on GPIO%d and GPIO%d: no memory for its task", tx, rx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* The task woken and ended, then the controller stopped. The wake: the task may be asleep in
 * canbus_receive() until its next timer, a second away, and canbus_wake() ends that sleep, or the next
 * one (node_task()). Every other wait of the task's is a tick at most (canbus_wait_room()) or a lock held
 * for a moment (canbus's, a log line's). A wake refused because the receive queue is full leaves the task
 * frames to read, so it doesn't sleep; it is sent again every tick until the task has ended anyway. */
static esp_err_t stop_locked(void)
{
    if (s_task != NULL) {
        s_running = false;
        do {
            canbus_wake(s_bus);
        } while (xSemaphoreTake(s_task_ended, pdMS_TO_TICKS(10)) != pdTRUE);
        s_task = NULL;
    }
    esp_err_t err = ESP_OK;
    if (s_bus != 0) {
        err = canbus_stop(s_bus);
        if (err == ESP_OK) {
            s_bus = 0;
        }
    }
#if CONFIG_CYPHAL_PNP_ALLOCATOR
    alloc_stop();       /* the task has ended: the allocator is off until the next start */
#endif
    if (err == ESP_OK) {
        s_state = CYPHAL_OFF;
    } else {
        /* The task has ended, so the node is stopped whatever the controller does. It still runs,
         * on the handle kept for the next try (`cyphal restart`), and the status says so */
        s_state = CYPHAL_FAILED;
        s_error = err;
        s_stop_failed = true;
    }
    return err;
}

esp_err_t cyphal_start(int tx_gpio, int rx_gpio)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const esp_err_t err = start_locked(tx_gpio, rx_gpio);
    /* A failed start gives the pins back (the application's caller then holds none), so a restart
     * mustn't take them */
    if (err == ESP_OK) {
        s_given = true;
    }
    unlock();
    return err;
}

esp_err_t cyphal_restart(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_given) {
        err = stop_locked();
        if (err == ESP_OK) {
            err = start_locked(s_tx, s_rx);
        }
        /* ESP_ERR_INVALID_STATE means never started; why a stop or start failed is in the status */
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_FAIL;
        }
    }
    unlock();
    return err;
}

void cyphal_status(cyphal_status_t *out)
{
    *out = (cyphal_status_t){ .state = CYPHAL_OFF, .tx_gpio = -1, .rx_gpio = -1, .heap_size = CONFIG_CYPHAL_HEAP_SIZE };
    if (s_lock == NULL) {
        return;
    }
    lock();
    out->state = s_state;
    out->error = s_error;
    out->stop_failed = s_stop_failed;
    out->running = s_run;
    out->saved = s_saved;
    out->node_id = s_node != NULL && s_state != CYPHAL_OFF ? s_node->node_id : s_run.node_id;
    /* With plug-and-play, ONLINE and IN_USE come only after an allocation */
    out->allocated = s_run.node_id == CYPHAL_NODE_ID_AUTO && (s_state == CYPHAL_ONLINE || s_state == CYPHAL_IN_USE);
    out->tx_gpio = s_tx;
    out->rx_gpio = s_rx;
    memcpy(out->unique_id, s_uid, sizeof(s_uid));
    out->pseudo_id = s_pseudo_id;
    if (s_task != NULL) {
        out->uptime_s = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        out->stack_unused = uxTaskGetStackHighWaterMark(s_task);   /* bytes, in ESP-IDF */
    }
    out->transfers_sent = s_tr_sent;
    out->transfers_received = s_tr_received;
    out->transfers_dropped = s_tr_dropped;
    out->frames_queued = s_frames_queued;
    const uint32_t at_ms = s_recover_at_ms;
    if (s_task != NULL && at_ms != 0) {
        const int32_t left_ms = (int32_t)(at_ms - (uint32_t)(esp_timer_get_time() / 1000));
        out->recover_in_s = left_ms > 0 ? (uint32_t)((left_ms + 999) / 1000) : 0;
    }
    if (s_heap != NULL) {
        out->heap_used = s_heap_free0 - multi_heap_free_size(s_heap);
        out->heap_peak = s_heap_free0 - multi_heap_minimum_free_size(s_heap);
    }
    out->heap_failed = s_heap_failed;
    out->services = s_services;
    out->job_slots = JOB_SLOTS;
    for (int i = 0; i < JOB_SLOTS; i++) {
        out->jobs_held += __atomic_load_n(&s_job_state[i], __ATOMIC_ACQUIRE) != JOB_FREE;
    }
    if (s_worker != NULL) {
        out->worker_stack_unused = uxTaskGetStackHighWaterMark(s_worker);
    }
    out->slow_worker = s_has_slow;
    out->slow_held = __atomic_load_n(&s_slow_held, __ATOMIC_ACQUIRE);
    if (s_slow != NULL) {
        out->slow_stack_unused = uxTaskGetStackHighWaterMark(s_slow);
    }
    out->txq_peak = s_txq_peak;
    out->txq_capacity = TX_QUEUE_FRAMES;
    out->health = s_health;
    for (size_t i = 0; i < s_mail_count; i++) {
        const mailbox_t *m = &s_mail[i];
        out->mail[i] = (cyphal_mail_t){ .port = m->port, .depth = m->depth, .published = m->published,
                                        .replaced = m->replaced, .refused = m->refused };
    }
    out->mail_count = s_mail_count;
    unlock();
}

#if CONFIG_CYPHAL_PNP_ALLOCATOR
/* ------------------------------------------------------------------ the plug-and-play allocator
 *
 * cyphal_alloc.h says what it does; the node calls it from the places the prototypes near the top name.
 *
 * Its table is shared three ways: the node's task reads it and adds rows (a request served, a node heard), the worker
 * copies it to save it, and the console lists and edits it. Every access to s_alloc_table and its rows is under
 * s_alloc_mux, a spinlock, held only for a pass over the rows (CONFIG_CYPHAL_ALLOCATOR_TABLE of them at most) and never
 * across anything that may wait (no log, no NVS, no libcanard), so the node's task never waits on the console or the
 * worker for longer than that pass. The node's task checks the allocator's state again inside that lock before it
 * adds a row, and a reset changes the state inside it, so no row is added after a reset has emptied the table.
 *
 * Every change to the table bumps s_alloc_changes; a save encodes the table and notes the count it saw, under the
 * spinlock, and records it in s_alloc_saved_changes only once NVS has committed, so "unsaved" (the two differ) holds
 * through a save under way and after one that failed. Saves are made under s_lock, one at a time: by the worker, for
 * rows the node's task added (retried after a failure, 1 s and then doubling up to 30 s apart, woken by a timer), and
 * by the console, for its edits; any save that commits ends the back-off. The node's task never writes NVS, and a
 * repeated request changes nothing. While the saved table is a later firmware's (s_alloc_newer) nothing is added or
 * saved, so that it isn't overwritten unread, until `cyphal allocations clear` or a reset.
 * The setting (s_alloc_saved) is written under s_lock and read anywhere. The state is changed under the spinlock
 * wherever a reset could come between its reading and its writing (the node's task, a reset), and by a start and a
 * stop, under s_lock with the task not running; the other allocator's node-ID and the counters are written by the
 * node's task (and by a start) and read whole.
 */

#define ALLOC_FIRST     CONFIG_CYPHAL_ALLOCATOR_FIRST
#define ALLOC_LAST      CONFIG_CYPHAL_ALLOCATOR_LAST
#define ALLOC_BLOB_MAX  CYPHAL_ALLOC_BLOB_SIZE(CONFIG_CYPHAL_ALLOCATOR_TABLE)
/* The node-IDs answered lately, remembered: twice the table, so that only forgets within 2 s could fill it with young
 * entries (each answer is a row's node-ID), and then nothing new is given rather than an entry forgotten */
#define ALLOC_RECENT    (2 * CONFIG_CYPHAL_ALLOCATOR_TABLE)
#define KEY_ALLOC_ON    "pnp_alloc"     /* u8: the setting, 1 on; missing, off */
#define KEY_ALLOC_TABLE "pnp_table"     /* blob: the table (cyphal_alloc_encode()); missing, empty */
/* An answer is not queued while the transmit queue holds more than this: 32 frames are kept for the node's own
 * traffic, a port list (22 frames), a heartbeat and a GetInfo response (7), so that a request storm can't make the
 * heartbeat's push fail. The allocatee asks again */
#define ALLOC_TXQ_MAX   (TX_QUEUE_FRAMES - 32)
#define ALLOC_RETRY_FIRST_US   1000000
#define ALLOC_RETRY_MAX_US     30000000
_Static_assert(ALLOC_FIRST >= 1 && ALLOC_LAST <= CYPHAL_NODE_ID_MAX && ALLOC_FIRST <= ALLOC_LAST,
               "CONFIG_CYPHAL_ALLOCATOR_FIRST..LAST is a range within 1..127");

static void on_alloc_request(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now);

/* NodeIDAllocationData, the allocator's, while it answers (CYPHAL_ALLOCATOR_ACTIVE): on n->allocation, which
 * plug-and-play's waiting never uses then (an allocator has a fixed node-ID). Not in s_rows[]: the node subscribes to
 * it only while it answers, and the port list names it then (alloc_port_lists()) */
static const service_t ALLOC_SERVE = {
    CYPHAL_PORT_SUB, uavcan_pnp_NodeIDAllocationData_1_0_FIXED_PORT_ID_, -1,
    uavcan_pnp_NodeIDAllocationData_1_0_EXTENT_BYTES_, on_alloc_request, NULL, NULL,
};

static portMUX_TYPE s_alloc_mux = portMUX_INITIALIZER_UNLOCKED;
static cyphal_alloc_table_t s_alloc_table;      /* its rows in PSRAM, from cyphal_init() */
static uint8_t *s_alloc_blob;                   /* the saved form's buffer, PSRAM, used under s_lock */
static volatile bool s_alloc_saved;             /* the setting, from the next start */
/* This start's: OFF unless the setting was on as it began, and OFF while the node doesn't run (alloc_stop()) */
static volatile cyphal_allocator_state_t s_alloc_state = CYPHAL_ALLOCATOR_OFF;
static volatile uint8_t s_alloc_other = CYPHAL_NODE_ID_AUTO;   /* YIELDED: the other allocator's node-ID */
static volatile uint32_t s_alloc_requests, s_alloc_allocations, s_alloc_repeats, s_alloc_refused, s_alloc_yielded,
    s_alloc_own, s_alloc_shed;
static uint32_t s_alloc_changes;                /* the table's changes, counted: through __atomic only */
static uint32_t s_alloc_saved_changes;          /* s_alloc_changes as last committed to NVS: through __atomic only */
static esp_timer_handle_t s_alloc_retry;        /* wakes the worker to retry a save that failed */
static int64_t s_alloc_retry_us;                /* under s_lock: the wait before the next retry, 0 after a success */
static int64_t s_alloc_retry_at;                /* under s_lock: the worker saves nothing before this */
/* The node task's, for this start */
static CanardTransferID s_alloc_tid;            /* the answers' transfer-ID */
static bool s_alloc_full_logged, s_alloc_own_logged;    /* a refusal logged this start */
static cyphal_alloc_recent_t s_alloc_recent;    /* node-IDs answered lately: taken even if their row goes. Its
                                                 * entries in PSRAM, from cyphal_init(); the node's task's */
static volatile bool s_alloc_newer;             /* the saved table is a later firmware's: written under s_lock */
static uint8_t *s_alloc_plain_list;             /* the port list without it, for once the allocator stops: PORT_LIST_MAX
                                                 * bytes in PSRAM, from cyphal_init() */
static size_t s_alloc_plain_list_size;

/* ---- the setting and the table */

typedef esp_err_t (*alloc_nvs_op_t)(nvs_handle_t h, const void *arg);

/* One change to the allocator's keys, committed */
static esp_err_t alloc_nvs(alloc_nvs_op_t op, const void *arg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = op(h, arg);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t put_table(nvs_handle_t h, const void *size)
{
    return nvs_set_blob(h, KEY_ALLOC_TABLE, s_alloc_blob, *(const size_t *)size);
}

static esp_err_t put_setting(nvs_handle_t h, const void *on)
{
    return nvs_set_u8(h, KEY_ALLOC_ON, *(const bool *)on ? 1 : 0);
}

/* A key gone, whether or not it was there */
static esp_err_t erase(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t erase_both(nvs_handle_t h, const void *arg)
{
    (void)arg;
    const esp_err_t err = erase(h, KEY_ALLOC_ON);
    return err == ESP_OK ? erase(h, KEY_ALLOC_TABLE) : err;
}

/* A change to the table, under the spinlock */
static void alloc_changed(void)
{
    __atomic_add_fetch(&s_alloc_changes, 1, __ATOMIC_RELEASE);
}

static bool alloc_unsaved(void)
{
    return __atomic_load_n(&s_alloc_changes, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&s_alloc_saved_changes, __ATOMIC_ACQUIRE);
}

/* The table into NVS, under s_lock: encoded under the spinlock with the change count it holds, written outside it,
 * and that count recorded as saved once committed, which also ends a failed save's back-off. Never over a later
 * firmware's table (cyphal_allocator_clear() lets it go first) */
static esp_err_t alloc_save_locked(void)
{
    if (s_alloc_newer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_alloc_blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_alloc_mux);
    const uint32_t changes = __atomic_load_n(&s_alloc_changes, __ATOMIC_ACQUIRE);
    const size_t size = cyphal_alloc_encode(&s_alloc_table, s_alloc_blob, ALLOC_BLOB_MAX);
    taskEXIT_CRITICAL(&s_alloc_mux);
    const esp_err_t err = size == 0 ? ESP_ERR_INVALID_SIZE : alloc_nvs(put_table, &size);
    if (err == ESP_OK) {
        __atomic_store_n(&s_alloc_saved_changes, changes, __ATOMIC_RELEASE);
        s_alloc_retry_us = 0;
        s_alloc_retry_at = 0;
    }
    return err;
}

/* The setting saved, under s_lock: from the node's next start */
static esp_err_t alloc_set_locked(bool on)
{
    const esp_err_t err = alloc_nvs(put_setting, &on);
    if (err == ESP_OK) {
        s_alloc_saved = on;
    }
    return err;
}

/* A reset's part, under s_lock: the setting off and the table empty, both keys erased (missing means off, and
 * empty), and this run's allocator stopped until the node's next start, in the same hold of the spinlock as the
 * table is emptied, so that the node's task neither answers nor adds a heard row after it */
static esp_err_t alloc_reset_locked(void)
{
    const esp_err_t err = alloc_nvs(erase_both, NULL);
    if (err == ESP_OK) {
        s_alloc_saved = false;
        s_alloc_newer = false;      /* its key is gone too */
        s_alloc_retry_us = 0;
        s_alloc_retry_at = 0;
        taskENTER_CRITICAL(&s_alloc_mux);
        cyphal_alloc_table_clear(&s_alloc_table);
        if (s_alloc_state != CYPHAL_ALLOCATOR_OFF) {
            s_alloc_state = CYPHAL_ALLOCATOR_RESET;
        }
        /* Nothing is left to save: the keys are gone and the table is empty */
        __atomic_store_n(&s_alloc_saved_changes, __atomic_load_n(&s_alloc_changes, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        taskEXIT_CRITICAL(&s_alloc_mux);
    }
    return err;
}

/* `cyphal reset` (cyphal_reset_settings()), after the node's own settings */
static esp_err_t alloc_reset(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const esp_err_t err = alloc_reset_locked();
    unlock();
    return err;
}

/* The register (CONFIG_CYPHAL_ALLOCATOR_REGISTER): one bit, the saved setting, applied at the node's next start. Kept
 * by this file, after the application's registers, so it has their rules (it can't be CYPHAL_REG_NEXT_START, which is
 * the node's own settings'; work_access() and `cyphal register` say "from the node's next start" for it by its write
 * function); its write runs under s_lock (cyphal_write_register()), on the worker or the console */
static void alloc_reg_read(const cyphal_register_t *r, const cyphal_settings_t *s, uavcan_register_Value_1_0 *out)
{
    (void)r;
    (void)s;
    const bool on = s_alloc_saved;
    cyphal_reg_set_bits(out, &on, 1);
}

static cyphal_reg_result_t alloc_reg_write(const cyphal_register_t *r, cyphal_settings_t *s,
                                           const uavcan_register_Value_1_0 *v, char *why, size_t why_len)
{
    (void)s;
    bool on = false;
    const cyphal_reg_result_t res = cyphal_reg_take_bits(r, v, &on, why, why_len);
    if (res != CYPHAL_REG_OK) {
        return res;
    }
    const esp_err_t err = alloc_set_locked(on);
    if (err != ESP_OK) {
        snprintf(why, why_len, "not saved: %s", esp_err_to_name(err));
        return CYPHAL_REG_BAD_VALUE;
    }
    return CYPHAL_REG_OK;
}

static const cyphal_register_t ALLOC_REGISTER = {
    .name = CONFIG_CYPHAL_ALLOCATOR_REGISTER, .type = CYPHAL_REG_BIT, .count = 1,
    .flags = CYPHAL_REG_MUTABLE | CYPHAL_REG_PERSISTENT, .read = alloc_reg_read, .write = alloc_reg_write,
};

/* Whether register r is the allocator's: applied at the node's next start, though not CYPHAL_REG_NEXT_START */
static bool alloc_is_register(const cyphal_register_t *r)
{
    return r != NULL && r->write == alloc_reg_write;
}

bool cyphal_allocator_is_register(size_t index)
{
    return alloc_is_register(cyphal_reg(index));
}

/* cyphal_init()'s cyphal_reg_init(), with the allocator's register after the application's (a copy of the two, kept).
 * An application port whose NVS key is one of the allocator's is refused as the node's own keys are (no application
 * port or register kept, the allocator's register kept); if the table can't keep the allocator's register, again
 * without it, so that its name can't cost the application its ports; and if the application's ports or registers
 * are what the table can't keep, the allocator's register alone, as for the key */
static bool alloc_reg_init(char *why, size_t why_len)
{
    for (size_t i = 0; s_app.ports != NULL && i < s_app.port_count; i++) {
        const char *key = s_app.ports[i].nvs_key;
        if (key != NULL && (strcmp(key, KEY_ALLOC_ON) == 0 || strcmp(key, KEY_ALLOC_TABLE) == 0)) {
            cyphal_reg_init(NULL, 0, CONFIG_CYPHAL_NAME_REGISTER, &ALLOC_REGISTER, 1, why, why_len);
            snprintf(why, why_len, "%s's NVS key %s is the allocator's", s_app.ports[i].id_register, key);
            return false;
        }
    }
    const size_t count = s_app.register_count;
    cyphal_register_t *all = psram_calloc((count + 1) * sizeof(*all));
    if (all != NULL) {
        if (count > 0) {
            memcpy(all, s_app.registers, count * sizeof(*all));
        }
        all[count] = ALLOC_REGISTER;
        if (cyphal_reg_init(s_app.ports, s_app.port_count, CONFIG_CYPHAL_NAME_REGISTER, all, count + 1, why, why_len)) {
            return true;
        }
        heap_caps_free(all);
    }
    const bool ok = cyphal_reg_init(s_app.ports, s_app.port_count, CONFIG_CYPHAL_NAME_REGISTER, s_app.registers, count,
                                    why, why_len);
    if (ok) {
        ESP_LOGE(TAG, "the allocator's register %s: %s; left out", CONFIG_CYPHAL_ALLOCATOR_REGISTER,
                 all == NULL ? "no memory" : "not a register name, or taken");
    } else {
        char again[64];     /* why stays the application's reason */
        cyphal_reg_init(NULL, 0, CONFIG_CYPHAL_NAME_REGISTER, &ALLOC_REGISTER, 1, again, sizeof(again));
    }
    return ok;
}

static void alloc_retry_cb(void *arg)
{
    (void)arg;
    cyphal_wake_worker();
}

/* The saved table read at its own size, into a buffer of its own: one saved by a build with a bigger table is kept as
 * far as it fits (cyphal_alloc_decode(): given rows first), not discarded */
static void alloc_load_table(nvs_handle_t h)
{
    size_t size = 0;
    if (nvs_get_blob(h, KEY_ALLOC_TABLE, NULL, &size) != ESP_OK) {
        return;     /* none saved */
    }
    uint8_t *blob = psram_calloc(size > 0 ? size : 1);
    size_t dropped = 0;
    unsigned version = 0;
    cyphal_alloc_blob_t r = CYPHAL_ALLOC_BLOB_BAD;
    if (blob != NULL && nvs_get_blob(h, KEY_ALLOC_TABLE, blob, &size) == ESP_OK) {
        r = cyphal_alloc_decode(&s_alloc_table, blob, size, &dropped);
        version = size > 0 ? blob[0] : 0;
    }
    heap_caps_free(blob);
    if (r == CYPHAL_ALLOC_BLOB_TRUNCATED) {
        ESP_LOGW(TAG, "allocator: the saved table has more rows than CONFIG_CYPHAL_ALLOCATOR_TABLE (%d): %u dropped, "
                 "heard ones first, and saved without them when a row is next added", CONFIG_CYPHAL_ALLOCATOR_TABLE,
                 (unsigned)dropped);
    } else if (r == CYPHAL_ALLOC_BLOB_NEWER) {
        s_alloc_newer = true;
        ESP_LOGW(TAG, "allocator: the saved table is a later firmware's (version %u): not read, and kept as it is; the "
                 "allocator adds, answers and saves nothing until `cyphal allocations clear` (which drops it) and the "
                 "node's next start", version);
    } else if (r == CYPHAL_ALLOC_BLOB_BAD) {
        ESP_LOGW(TAG, "allocator: the saved table (%u bytes) isn't one: empty, until a row added saves over it",
                 (unsigned)size);
    }
}

/* cyphal_init(): the setting and the table as saved */
static void alloc_load(void)
{
    cyphal_alloc_row_t *rows = psram_calloc(CONFIG_CYPHAL_ALLOCATOR_TABLE * sizeof(cyphal_alloc_row_t));
    s_alloc_blob = psram_calloc(ALLOC_BLOB_MAX);
    s_alloc_plain_list = psram_calloc(PORT_LIST_MAX);
    cyphal_alloc_recent_entry_t *recent = psram_calloc(ALLOC_RECENT * sizeof(*recent));
    cyphal_alloc_table_init(&s_alloc_table, rows, CONFIG_CYPHAL_ALLOCATOR_TABLE);
    cyphal_alloc_recent_init(&s_alloc_recent, recent, ALLOC_RECENT);
    const esp_timer_create_args_t retry = { .callback = alloc_retry_cb, .name = "cyphal_alloc" };
    /* What each failure costs, said once */
    if (rows == NULL) {
        ESP_LOGE(TAG, "allocator: no memory for its table: it answers nothing");
    } else if (recent == NULL) {
        ESP_LOGE(TAG, "allocator: no memory for the node-IDs answered lately: it answers its rows but gives no new "
                 "node-ID");
    }
    if (rows != NULL && s_alloc_blob == NULL) {
        ESP_LOGE(TAG, "allocator: no memory for the saved table's buffer: it answers, but saves no row");
    }
    if (esp_timer_create(&retry, &s_alloc_retry) != ESP_OK) {
        s_alloc_retry = NULL;
        ESP_LOGE(TAG, "allocator: no timer: a failed save is tried again only when the worker next wakes for "
                 "something else");
    }
    if (s_alloc_plain_list == NULL) {
        ESP_LOGE(TAG, "allocator: no memory for the second port list: the port list leaves out subject 8166");
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t on = 0;
    if (nvs_get_u8(h, KEY_ALLOC_ON, &on) == ESP_OK) {
        s_alloc_saved = on == 1;
        if (on > 1) {
            ESP_LOGW(TAG, "allocator: the saved setting %u isn't 0 or 1: off", on);
        }
    }
    if (rows != NULL) {
        alloc_load_table(h);
    }
    nvs_close(h);
}

esp_err_t cyphal_set_allocator(bool on)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const esp_err_t err = alloc_set_locked(on);
    unlock();
    return err;
}

size_t cyphal_allocator_rows(cyphal_alloc_row_t *out, size_t max)
{
    taskENTER_CRITICAL(&s_alloc_mux);
    const size_t n = s_alloc_table.count < max ? s_alloc_table.count : max;
    if (n > 0) {
        memcpy(out, s_alloc_table.rows, n * sizeof(*out));
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    return n;
}

esp_err_t cyphal_allocator_forget(uint8_t node_id)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    taskENTER_CRITICAL(&s_alloc_mux);
    const bool forgot = cyphal_alloc_table_forget(&s_alloc_table, node_id);
    if (forgot) {
        alloc_changed();
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    const esp_err_t err = forgot ? alloc_save_locked() : ESP_ERR_NOT_FOUND;
    unlock();
    return err;
}

esp_err_t cyphal_allocator_clear(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (s_alloc_newer) {
        ESP_LOGW(TAG, "allocator: the later firmware's saved table dropped (`cyphal allocations clear`); allocating "
                 "from the node's next start");
        s_alloc_newer = false;      /* the operator's word: saved over from here */
    }
    taskENTER_CRITICAL(&s_alloc_mux);
    cyphal_alloc_table_clear(&s_alloc_table);
    alloc_changed();
    taskEXIT_CRITICAL(&s_alloc_mux);
    const esp_err_t err = alloc_save_locked();
    unlock();
    return err;
}

void cyphal_allocator_status(cyphal_allocator_status_t *out)
{
    *out = (cyphal_allocator_status_t){ .first = ALLOC_FIRST, .last = ALLOC_LAST, .other = CYPHAL_NODE_ID_AUTO };
    if (s_lock == NULL) {
        return;
    }
    lock();
    out->saved = s_alloc_saved;
    out->newer_table = s_alloc_newer;
    if (s_task != NULL) {       /* running: this start's state */
        out->state = s_alloc_state;
        out->running = s_alloc_state != CYPHAL_ALLOCATOR_OFF && s_alloc_state != CYPHAL_ALLOCATOR_RESET;
        out->other = s_alloc_other;
    }
    out->requests = s_alloc_requests;
    out->allocations = s_alloc_allocations;
    out->repeats = s_alloc_repeats;
    out->refused = s_alloc_refused;
    out->yielded = s_alloc_yielded;
    out->own = s_alloc_own;
    out->shed = s_alloc_shed;
    unlock();
    taskENTER_CRITICAL(&s_alloc_mux);
    out->capacity = s_alloc_table.capacity;
    out->rows = s_alloc_table.count;
    for (size_t i = 0; i < s_alloc_table.count; i++) {
        out->heard_rows += (s_alloc_table.rows[i].flags & CYPHAL_ALLOC_ROW_HEARD) != 0;
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    out->unsaved = alloc_unsaved();
}

/* ---- the worker's */

/* Every turn: the table saved if it has changed since the last commit and no retry is pending. Looked at again under
 * s_lock, since a reset or a console save may have come since (a save queued before a reset mustn't make the erased
 * key again). A failure is retried, 1 s and then doubling up to 30 s apart (the timer wakes the worker), so a broken
 * NVS costs a write a while, not a spinning worker; one warning a run of failures */
static void alloc_worker(void)
{
    if (!alloc_unsaved()) {
        return;
    }
    lock();
    const int64_t now = esp_timer_get_time();
    if (!alloc_unsaved() || s_alloc_newer || now < s_alloc_retry_at) {
        unlock();
        return;
    }
    const esp_err_t err = alloc_save_locked();
    const bool first = err != ESP_OK && s_alloc_retry_us == 0;
    if (err != ESP_OK) {
        s_alloc_retry_us = s_alloc_retry_us == 0 ? ALLOC_RETRY_FIRST_US
                           : s_alloc_retry_us * 2 > ALLOC_RETRY_MAX_US ? ALLOC_RETRY_MAX_US
                                                                      : s_alloc_retry_us * 2;
        s_alloc_retry_at = now + s_alloc_retry_us;
        if (s_alloc_retry != NULL) {
            esp_timer_stop(s_alloc_retry);
            esp_timer_start_once(s_alloc_retry, (uint64_t)s_alloc_retry_us);
        }
    }
    unlock();
    if (first) {
        ESP_LOGW(TAG, "allocator: the table wasn't saved (%s): trying again, ever less often", esp_err_to_name(err));
    }
}

/* ---- the node's task's */

/* start_locked(), under s_lock, before the task exists: the setting as saved now, for this run. Off; on with
 * plug-and-play, which never allocates (an allocator needs a fixed node-ID); or on, quiet through the listen
 * (alloc_online() starts it) */
static void alloc_start(void)
{
    s_alloc_requests = s_alloc_allocations = s_alloc_repeats = s_alloc_refused = s_alloc_yielded = 0;
    s_alloc_own = s_alloc_shed = 0;
    s_alloc_other = CYPHAL_NODE_ID_AUTO;
    s_alloc_tid = 0;
    s_alloc_full_logged = s_alloc_own_logged = false;
    s_alloc_state = !s_alloc_saved                         ? CYPHAL_ALLOCATOR_OFF
                    : s_run.node_id == CYPHAL_NODE_ID_AUTO ? CYPHAL_ALLOCATOR_NEEDS_FIXED
                    : s_alloc_newer                        ? CYPHAL_ALLOCATOR_NEWER_TABLE
                                                           : CYPHAL_ALLOCATOR_LISTENING;
    if (s_alloc_state == CYPHAL_ALLOCATOR_NEEDS_FIXED) {
        ESP_LOGW(TAG, "allocator: on, but the node-ID is plug-and-play: an allocator needs a fixed node-ID "
                 "(`cyphal id <n>`), so it gives none");
    } else if (s_alloc_state == CYPHAL_ALLOCATOR_NEWER_TABLE) {
        ESP_LOGW(TAG, "allocator: on, but the saved table is a later firmware's: it answers nothing, so as not to save "
                 "over it, until `cyphal allocations clear` and the node's next start");
    }
}

/* stop_locked(), the task ended: off until the next start, whatever stopped it */
static void alloc_stop(void)
{
    taskENTER_CRITICAL(&s_alloc_mux);
    s_alloc_state = CYPHAL_ALLOCATOR_OFF;
    taskEXIT_CRITICAL(&s_alloc_mux);
}

/* start_locked(), after serialize_port_list(): when the allocator will answer this run (on, a fixed node-ID, no later
 * firmware's table), the port list made again with subject 8166 published and subscribed, and the one without it
 * kept, for port_list() to send once the allocator stops (it yields, the node stops, a reset). If the second can't be
 * made (bigger than PORT_LIST_MAX, no memory), the plain one is the only one, logged once: never a failed start */
static esp_err_t alloc_port_lists(node_t *n)
{
    static bool logged;
    s_alloc_plain_list_size = 0;    /* none: n->port_list is the only one this run */
    if (!s_alloc_saved || s_run.node_id == CYPHAL_NODE_ID_AUTO || s_alloc_newer || s_alloc_plain_list == NULL) {
        return ESP_OK;
    }
    const size_t plain = n->port_list_size;
    memcpy(s_alloc_plain_list, n->port_list, plain);
    s_alloc_port_listed = true;
    const esp_err_t err = serialize_port_list(n);      /* n->port_list as it was unless it succeeds */
    s_alloc_port_listed = false;
    if (err == ESP_OK) {
        s_alloc_plain_list_size = plain;
    } else if (!logged) {
        logged = true;
        ESP_LOGW(TAG, "allocator: the port list with subject 8166 couldn't be made (%s): the plain one is sent",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

/* port_list(): the list to send now. With the allocator answering, its subject is in it */
static const uint8_t *alloc_port_list(const node_t *n, size_t *size)
{
    if (s_alloc_plain_list_size == 0 || s_alloc_state == CYPHAL_ALLOCATOR_ACTIVE) {
        *size = n->port_list_size;
        return n->port_list;
    }
    *size = s_alloc_plain_list_size;
    return s_alloc_plain_list;
}

/* go_online(): quiet through the listen, answering from now, unless another allocator was heard in it or a reset
 * came. The state moves under the spinlock, where a reset changes it, so that a reset's RESET is never overwritten;
 * subscribed only once it has moved (a reset after that leaves the subscription, which on_alloc_request() ignores) */
static void alloc_online(node_t *n)
{
    taskENTER_CRITICAL(&s_alloc_mux);
    const bool answer = s_alloc_state == CYPHAL_ALLOCATOR_LISTENING;
    if (answer) {
        s_alloc_state = CYPHAL_ALLOCATOR_ACTIVE;
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    if (!answer) {
        return;
    }
    canardRxSubscribe(&n->canard, CanardTransferKindMessage, ALLOC_SERVE.port_id, ALLOC_SERVE.extent,
                      CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, &n->allocation);
    n->allocation.user_reference = (void *)&ALLOC_SERVE;
    ESP_LOGI(TAG, "allocator: answering plug-and-play requests with node-IDs %d to %d", ALLOC_FIRST, ALLOC_LAST);
}

/* stop_in_use(): the node sends nothing now, and on_frame() no longer hands frames to libcanard. Under the spinlock,
 * so that a reset's RESET stays the reason */
static void alloc_in_use(void)
{
    taskENTER_CRITICAL(&s_alloc_mux);
    if (s_alloc_state == CYPHAL_ALLOCATOR_LISTENING || s_alloc_state == CYPHAL_ALLOCATOR_ACTIVE) {
        s_alloc_state = CYPHAL_ALLOCATOR_STOPPED;
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
}

/* Whether the allocator may change the table now: in the listen or answering. Read under the spinlock, where a reset
 * changes it */
static bool alloc_may_add(void)
{
    return s_alloc_state == CYPHAL_ALLOCATOR_LISTENING || s_alloc_state == CYPHAL_ALLOCATOR_ACTIVE;
}

typedef struct {
    const node_t *n;
    int64_t now;
    bool recent_full;       /* cyphal_alloc_recent_full() as of now: taken once, before the spinlock */
} alloc_busy_t;

/* cyphal_alloc_pick()'s "in use": heard within a heartbeat's offline timeout (the node's cyphal_seen_t, fed here), or
 * answered within CYPHAL_ALLOC_RECENT_US (an allocatee holds its allocation about 1 s before it is heard). With every
 * entry of the answered-lately list young, every node-ID counts as in use: a new one couldn't be remembered, and
 * forgetting a young one would reopen its 2 s (so the request is refused, counted) */
static bool alloc_busy(void *ctx, uint8_t node_id)
{
    const alloc_busy_t *b = ctx;
    return b->recent_full || cyphal_pnp_in_use(&b->n->seen, node_id, b->now) ||
           cyphal_alloc_recent_has(&s_alloc_recent, node_id, b->now);
}

/* on_frame(), every frame, before libcanard sees it, while the allocator may answer (on, the fixed node-ID listening
 * or online). Another allocator's answer ends the allocating until the next start (one allocator a bus: plug-and-play
 * v1 has no arbitration between them), read from the frame alone, so that it is seen in the listen too, and before
 * the subscription could take the answer. Every source is remembered (cyphal_seen_t, for "in use"); a source in the
 * range with no row gets a heard row (the spec's mock entry). So a `clear` while nodes are online in the range makes
 * each of them a heard row within its next heartbeat: spec-correct, and their pseudo-IDs' node-IDs are forgotten */
static void alloc_frame(node_t *n, const canbus_frame_t *f, cyphal_state_t state, int64_t stamp)
{
    const cyphal_allocator_state_t a = s_alloc_state;
    if ((a != CYPHAL_ALLOCATOR_LISTENING && a != CYPHAL_ALLOCATOR_ACTIVE) ||
        (state != CYPHAL_LISTENING && state != CYPHAL_ONLINE)) {
        return;
    }
    uint8_t other = 0;
    if (cyphal_alloc_other_allocator(f->id, f->ext, f->rtr, f->data, f->len, n->node_id, &other)) {
        if (a == CYPHAL_ALLOCATOR_ACTIVE) {
            canardRxUnsubscribe(&n->canard, CanardTransferKindMessage, ALLOC_SERVE.port_id);
        }
        s_alloc_other = other;
        s_alloc_yielded++;
        taskENTER_CRITICAL(&s_alloc_mux);
        if (alloc_may_add()) {      /* not over a reset's RESET */
            s_alloc_state = CYPHAL_ALLOCATOR_YIELDED;
        }
        taskEXIT_CRITICAL(&s_alloc_mux);
        ESP_LOGW(TAG, "allocator: node %u answers plug-and-play requests too: not allocating until the node's next "
                 "start (one allocator a bus)", other);
        return;
    }
    const int src = cyphal_frame_source(f->id, f->ext, f->rtr, f->len);
    if (src < 0 || src == n->node_id) {     /* our own node-ID is a collision: on_frame()'s */
        return;
    }
    cyphal_seen_frame(&n->seen, src, stamp);
    if (src < ALLOC_FIRST || src > ALLOC_LAST) {
        return;
    }
    taskENTER_CRITICAL(&s_alloc_mux);
    const bool added = alloc_may_add() &&
                       cyphal_alloc_heard(&s_alloc_table, (uint8_t)src, ALLOC_FIRST, ALLOC_LAST, n->node_id);
    if (added) {
        alloc_changed();
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    if (added) {
        cyphal_wake_worker();
        ESP_LOGI(TAG, "allocator: node %d heard on the bus with no row: kept as heard, never given", src);
    }
}

/* A NodeIDAllocationData received while answering: a request (anonymous, well formed) is answered at once, from its
 * row or a new one, with a regular message carrying the same pseudo-ID and the row's node-ID; a new row is saved by
 * the worker. No answer for want of room (the spec's rule), for a row with the node's own node-ID, or while the
 * transmit queue is nearly full (ALLOC_TXQ_MAX): each counted, the first two logged once a start */
static void on_alloc_request(node_t *n, const service_t *svc, const struct CanardRxTransfer *t, int64_t now)
{
    (void)svc;
    uint64_t pseudo = 0;
    if (s_alloc_state != CYPHAL_ALLOCATOR_ACTIVE ||
        !cyphal_alloc_request(t->payload.data, t->payload.size, t->metadata.remote_node_id == CANARD_NODE_ID_UNSET,
                              &pseudo)) {
        return;
    }
    s_alloc_requests++;
    if (n->txq.size > ALLOC_TXQ_MAX) {
        s_alloc_shed++;
        return;
    }
    alloc_busy_t busy = { n, now, cyphal_alloc_recent_full(&s_alloc_recent, now) };
    uint8_t id = 0;
    cyphal_alloc_result_t r = CYPHAL_ALLOC_FULL;
    bool stopped = false;
    taskENTER_CRITICAL(&s_alloc_mux);
    if (alloc_may_add()) {
        r = cyphal_alloc_serve(&s_alloc_table, pseudo, ALLOC_FIRST, ALLOC_LAST, n->node_id, alloc_busy, &busy, &id);
        if (r == CYPHAL_ALLOC_NEW) {
            alloc_changed();
        }
    } else {
        stopped = true;     /* a reset came between */
    }
    taskEXIT_CRITICAL(&s_alloc_mux);
    if (stopped) {
        return;
    }
    if (r == CYPHAL_ALLOC_OWN) {
        s_alloc_own++;
        if (!s_alloc_own_logged) {
            s_alloc_own_logged = true;
            ESP_LOGW(TAG, "allocator: pseudo-ID %012llx's row has node-ID %u, this node's own: not answered "
                     "(`cyphal allocations forget %u`)", (unsigned long long)pseudo, id, id);
        }
        return;
    }
    if (r == CYPHAL_ALLOC_FULL || r == CYPHAL_ALLOC_NO_ID) {
        s_alloc_refused++;
        if (!s_alloc_full_logged) {
            s_alloc_full_logged = true;
            ESP_LOGW(TAG, "allocator: no node-ID for pseudo-ID %012llx: %s; not answered (and not logged again this "
                     "start)", (unsigned long long)pseudo,
                     r == CYPHAL_ALLOC_FULL ? "the table is full (`cyphal allocations`)"
                                            : "every node-ID in the range is taken, in use or answered lately");
        }
        return;
    }
    if (r == CYPHAL_ALLOC_NEW) {
        s_alloc_allocations++;
        cyphal_wake_worker();
        ESP_LOGI(TAG, "allocator: node-ID %u given to pseudo-ID %012llx", id, (unsigned long long)pseudo);
    } else {
        s_alloc_repeats++;
    }
    /* Never false for a new row's node-ID (alloc_busy() refused one that couldn't be noted); for a known row's, false
     * leaves its row to protect it */
    (void)cyphal_alloc_recent_note(&s_alloc_recent, id, now);
    uint8_t buf[CYPHAL_ALLOC_REPLY_SIZE];
    const size_t size = cyphal_alloc_reply(buf, sizeof(buf), pseudo, id);
    if (size == 0) {        /* never: every row's node-ID is 0-127 */
        return;
    }
    /* The spec gives no priority for the answer: nominal, as the kit's own requests and the heartbeat */
    const struct CanardTransferMetadata meta = {
        .priority = CanardPriorityNominal,
        .transfer_kind = CanardTransferKindMessage,
        .port_id = ALLOC_SERVE.port_id,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = s_alloc_tid++,
    };
    push(n, &meta, buf, size, now);
}
#endif /* CONFIG_CYPHAL_PNP_ALLOCATOR */
