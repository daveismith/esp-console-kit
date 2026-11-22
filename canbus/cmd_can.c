/*
 * `can`: the bench command over canbus.h. A task of its own reads the receive queue, sends back
 * what `can echo` asks for and prints what `can dump` asks for. It is made at each `can start`
 * and ends at `can stop`, before the controller stops, so it holds no stack while `can` is
 * stopped and never takes frames meant for another user of canbus_receive().
 *
 * While another owner (the Cyphal node) holds the controller, `can` alone reports and every
 * sub-command refuses: canbus would refuse them anyway, since `can` has no handle for that run.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "canbus.h"
#include "cmd_can.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* The reader's stack, priority and core: Kconfig (CANBUS_CMD_TASK_*), whose help has the reasons */
#define TASK_STACK  CONFIG_CANBUS_CMD_TASK_STACK
#define TASK_PRIO   CONFIG_CANBUS_CMD_TASK_PRIO
#define TASK_CORE   CONFIG_CANBUS_CMD_TASK_CORE
#define SEND_WAIT   pdMS_TO_TICKS(100)
/* How often the reader looks at s_reading: what `can stop` waits for it, at most (one echo's
 * SEND_WAIT more if it is sending) */
#define READ_WAIT   pdMS_TO_TICKS(50)

/* canbus_config_t.owner for the runs `can start` begins */
static const char OWNER[] = "the `can` command";

static can_cmd_config_t s_cfg;
static canbus_handle_t s_bus;           /* from `can start` to `can stop`; 0 while `can` holds nothing */
static TaskHandle_t s_task;             /* the reader, from `can start` to `can stop` */
static SemaphoreHandle_t s_task_ended;  /* given by the reader as it ends */
static volatile bool s_reading;         /* the reader's to go on; `can stop` clears it */
static volatile bool s_dump, s_echo;
static volatile uint32_t s_echo_failed;
static volatile canbus_mode_t s_mode;

static void print_frame(const canbus_frame_t *f)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "can: %0*lX [%u]", f->ext ? 8 : 3, (unsigned long)f->id, f->len);
    if (f->rtr) {
        n += snprintf(line + n, sizeof(line) - n, " remote");
    } else {
        for (int i = 0; i < f->len; i++) {
            n += snprintf(line + n, sizeof(line) - n, " %02X", f->data[i]);
        }
    }
    /* On the reader task, not the console's: the application's hook if it gave one */
    if (s_cfg.print != NULL) {
        s_cfg.print(line);
    } else {
        printf("%s\n", line);
    }
}

static void bench_task(void *arg)
{
    (void)arg;
    canbus_frame_t f;
    while (s_reading) {
        if (!canbus_receive(s_bus, &f, READ_WAIT)) {
            continue;
        }
        /* The echo first, so it doesn't wait behind the console. Not in a self-test, where it
         * would echo its own echoes. */
        if (s_echo && s_mode == CANBUS_NORMAL) {
            canbus_frame_t e = f;
            e.id = (e.id + 1) & (e.ext ? CANBUS_EXT_ID_MAX : CANBUS_STD_ID_MAX);
            if (canbus_send(s_bus, &e, SEND_WAIT) != ESP_OK) {
                s_echo_failed++;
            }
        }
        if (s_dump) {
            print_frame(&f);
        }
    }
    xSemaphoreGive(s_task_ended);
    vTaskDelete(NULL);
}

/* The reader made, after canbus_start(): before it, canbus_receive() has no queue to wait on */
static bool start_reader(void)
{
    s_reading = true;
    if (xTaskCreatePinnedToCore(bench_task, "can", TASK_STACK, NULL, TASK_PRIO, &s_task, TASK_CORE) != pdPASS) {
        s_reading = false;
        s_task = NULL;
        return false;
    }
    return true;
}

/* The reader ended, before canbus_stop(): once this returns it takes no more frames */
static void stop_reader(void)
{
    if (s_task == NULL) {
        return;
    }
    s_reading = false;
    xSemaphoreTake(s_task_ended, portMAX_DELAY);
    s_task = NULL;
}

/* True, having said so, while someone other than `can` holds the controller */
static bool held_by_other(void)
{
    const char *owner = canbus_owner();
    if (s_bus != 0 || owner == NULL) {
        return false;
    }
    printf("can: the controller is in use by %s: `can` only reports while it is\n", owner);
    return true;
}

static void print_status(void)
{
    canbus_status_t st;
    canbus_status(&st);
    static const char *const MODES[] = { "normal", "listen-only", "self-test" };
    if (st.state == CANBUS_STOPPED) {
        printf("can: stopped\n");
    } else {
        char rate[24];
        printf("can: running at %s on GPIO%d (TX), GPIO%d (RX), %s",
               canbus_format_bitrate(rate, sizeof(rate), st.config.bitrate), st.config.tx_gpio, st.config.rx_gpio,
               MODES[st.config.mode]);
        if (s_bus == 0 && st.config.owner != NULL) {
            printf(", for %s", st.config.owner);
        }
        printf("\n");
        printf("  bus: %s, tx errors %u, rx errors %u, bus errors %lu, bus-off %lu times%s\n",
               canbus_state_name(st.state), st.tx_errors, st.rx_errors, (unsigned long)st.bus_errors,
               (unsigned long)st.bus_off,
               /* On the S3 the driver sets REC to 128 for listen-only (an erratum: it must never
                * send a dominant error flag), and it stays there. The P4 has no such erratum and
                * reads 0, so the note is the S3's alone */
#if CONFIG_IDF_TARGET_ESP32S3
               st.config.mode == CANBUS_LISTEN ? " (listen-only always reads so)" : "");
#else
               "");
#endif
    }
    printf("  frames: sent %lu, received %lu; send failed %lu, receive dropped %lu (queue held %lu of %d at most), "
           "waiting to send %lu\n", (unsigned long)st.tx_frames, (unsigned long)st.rx_frames,
           (unsigned long)st.tx_failed, (unsigned long)st.rx_dropped, (unsigned long)st.rx_queue_peak,
           CONFIG_CANBUS_RX_QUEUE_LEN, (unsigned long)st.tx_pending);
    printf("  dump %s, echo %s", s_dump ? "on" : "off", s_echo ? "on" : "off");
    if (s_echo_failed) {
        printf(" (%lu not echoed)", (unsigned long)s_echo_failed);
    }
    printf("; internal heap %u bytes free, %u lowest\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static int hexval(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : isxdigit((unsigned char)c) ? (tolower((unsigned char)c) - 'a' + 10) : -1;
}

/* <id>#<hex>: three hex digits or fewer for an 11-bit ID, eight for a 29-bit one */
static const char *parse_frame(const char *s, canbus_frame_t *f)
{
    *f = (canbus_frame_t){ 0 };
    const char *hash = strchr(s, '#');
    if (hash == NULL) {
        return "write it <id>#<hex>, as 123#DEADBEEF";
    }
    const size_t idlen = (size_t)(hash - s);
    if (idlen == 0 || (idlen > 3 && idlen != 8)) {
        return "an ID is up to three hex digits (11-bit) or eight (29-bit)";
    }
    for (size_t i = 0; i < idlen; i++) {
        const int v = hexval(s[i]);
        if (v < 0) {
            return "the ID is not hex";
        }
        f->id = f->id << 4 | (uint32_t)v;
    }
    f->ext = idlen == 8;
    if (f->id > (f->ext ? CANBUS_EXT_ID_MAX : CANBUS_STD_ID_MAX)) {
        return f->ext ? "a 29-bit ID is 1FFFFFFF at most" : "an 11-bit ID is 7FF at most";
    }
    const char *d = hash + 1;
    const size_t dlen = strlen(d);
    if (dlen % 2 != 0 || dlen > 16) {
        return "the data is up to 8 bytes, two hex digits each";
    }
    for (size_t i = 0; i < dlen; i += 2) {
        const int hi = hexval(d[i]), lo = hexval(d[i + 1]);
        if (hi < 0 || lo < 0) {
            return "the data is not hex";
        }
        f->data[f->len++] = (uint8_t)(hi << 4 | lo);
    }
    return NULL;
}

static int can_start(int argc, char **argv)
{
    canbus_config_t c = { .mode = CANBUS_NORMAL, .owner = OWNER };
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            if (!canbus_parse_bitrate(argv[++i], &c.bitrate)) {
                printf("can: not a bitrate: %s (1000000, 500k, 1m; 10k to 1m)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--listen") == 0 && c.mode == CANBUS_NORMAL) {
            c.mode = CANBUS_LISTEN;
        } else if (strcmp(argv[i], "--selftest") == 0 && c.mode == CANBUS_NORMAL) {
            c.mode = CANBUS_SELFTEST;
        } else {
            printf("usage: can start [-b <bitrate>] [--listen|--selftest]\n");
            return 1;
        }
    }
    if (held_by_other()) {
        return 1;
    }
    if (s_bus != 0) {
        printf("can: already running (`can stop` first)\n");
        return 1;
    }
    const char *why = s_cfg.pins(&c.tx_gpio, &c.rx_gpio);
    if (why != NULL) {
        printf("can: not started on GPIO%d and GPIO%d: %s\n", c.tx_gpio, c.rx_gpio, why);
        return 1;
    }
    s_mode = c.mode;    /* before the first frame can reach the task */
    const esp_err_t err = canbus_start(&c, &s_bus);
    if (err != ESP_OK) {
        printf("can: not started: %s\n", err == ESP_ERR_NOT_FOUND ? "the controller is in use" :
               err == ESP_ERR_INVALID_ARG ? "the controller can't do that bitrate on those pins" : esp_err_to_name(err));
        return 1;
    }
    if (!start_reader()) {
        canbus_stop(s_bus);
        s_bus = 0;
        printf("can: no memory for its task\n");
        return 1;
    }
    s_echo_failed = 0;
    if (s_cfg.on_pins != NULL) {
        s_cfg.on_pins(c.tx_gpio, c.rx_gpio, true);
    }
    print_status();
    return 0;
}

static int can_send(const char *arg)
{
    canbus_frame_t f;
    const char *why = parse_frame(arg, &f);
    if (why != NULL) {
        printf("can: %s\n", why);
        return 1;
    }
    const esp_err_t err = canbus_send(s_bus, &f, SEND_WAIT);
    if (err == ESP_OK) {
        return 0;
    }
    canbus_status_t st;
    canbus_status(&st);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        printf("can: listen-only: it sends nothing\n");
    } else if (err == ESP_ERR_TIMEOUT) {
        printf("can: not sent: %lu frames are already waiting, %s (tx errors %u)\n", (unsigned long)st.tx_pending,
               canbus_state_name(st.state), st.tx_errors);
    } else if (st.state == CANBUS_BUS_OFF) {
        printf("can: bus-off: `can recover` first\n");
    } else if (st.state == CANBUS_STOPPED) {
        printf("can: not running (`can start`)\n");
    } else {
        printf("can: not sent: %s\n", esp_err_to_name(err));
    }
    return 1;
}

/* `on`, `off` or nothing (on); false for anything else */
static bool on_off(int argc, char **argv, volatile bool *flag)
{
    if (argc < 3 || strcmp(argv[2], "on") == 0) {
        *flag = true;
    } else if (strcmp(argv[2], "off") == 0) {
        *flag = false;
    } else {
        return false;
    }
    return true;
}

static int cmd_can(int argc, char **argv)
{
    if (argc == 1) {
        print_status();
        return 0;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "start") == 0) {
        return can_start(argc, argv);
    }
    if (held_by_other()) {
        return 1;
    }
    if (strcmp(sub, "stop") == 0 && argc == 2) {
        canbus_status_t st;
        canbus_status(&st);
        /* A later `can start` begins quiet: it neither sends nor prints until asked again */
        s_echo = s_dump = false;
        stop_reader();
        if (s_bus == 0) {
            printf("can: not running\n");
            return 0;
        }
        const esp_err_t err = canbus_stop(s_bus);
        if (err != ESP_OK) {
            /* Still running, so the pins stay reserved; the reader is gone, so start it again */
            if (!start_reader()) {
                printf("can: no memory for its task\n");
            }
            printf("can: not stopped: %s (try again)\n", esp_err_to_name(err));
            return 1;
        }
        s_bus = 0;
        if (s_cfg.on_pins != NULL) {
            s_cfg.on_pins(st.config.tx_gpio, st.config.rx_gpio, false);
        }
        printf("can: stopped; GPIO%d and GPIO%d released\n", st.config.tx_gpio, st.config.rx_gpio);
        return 0;
    }
    if (strcmp(sub, "send") == 0 && argc == 3) {
        return can_send(argv[2]);
    }
    if (strcmp(sub, "dump") == 0 && argc <= 3 && on_off(argc, argv, &s_dump)) {
        printf("can: %s\n", s_dump ? "printing every frame received (`can dump off` stops)" : "not printing");
        return 0;
    }
    if (strcmp(sub, "echo") == 0 && argc <= 3 && on_off(argc, argv, &s_echo)) {
        s_echo_failed = 0;
        printf("can: %s\n", s_echo ? "sending every frame received back, with 1 added to its ID "
                                     "(normal mode only; `can echo off` stops)" : "not echoing");
        return 0;
    }
    if (strcmp(sub, "recover") == 0 && argc == 2) {
        const esp_err_t err = canbus_recover(s_bus);
        if (err != ESP_OK) {
            printf("can: %s\n", canbus_running() ? "not bus-off: nothing to recover from" : "not running");
            return 1;
        }
        printf("can: recovering: back on the bus after 128 idle periods of 11 bits\n");
        return 0;
    }
    printf("usage: can [start [-b <bitrate>] [--listen|--selftest] | stop | send <id>#<hex> | dump [on|off] | "
           "echo [on|off] | recover]\n");
    return 1;
}

void register_can_commands(const can_cmd_config_t *config)
{
    s_cfg = *config;
    ESP_ERROR_CHECK(canbus_init());
    s_task_ended = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_task_ended ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_console_cmd_t cmd = {
        .command = "can",
        .help = "Raw CAN frames on the host link's pins, for testing a transceiver and a bus. Alone, its state, "
                "error counters and frame counts. `start` takes the pins (1 Mbit/s unless -b; --listen never "
                "drives the bus, --selftest receives its own frames); `stop` gives them back. `send 123#DEADBEEF` "
                "sends one frame (eight ID digits for 29 bits); `dump` prints what arrives; `echo` sends it back "
                "on the next ID (both off again at `stop`); `recover` brings it back from bus-off.",
        .hint = "[start [-b <bitrate>] [--listen|--selftest] | stop | send <id>#<hex> | dump [on|off] | echo [on|off] | "
                "recover]",
        .func = cmd_can,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
