/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Console example — various system commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#if CONFIG_SPI_FLASH_ENABLE_COUNTERS
#include "esp_spi_flash_counters.h"
#endif
#include "driver/gpio.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cmd_system.h"
#include "sdkconfig.h"

#ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
#define WITH_TASKS_INFO 1
#endif

static const char *TAG = "cmd_system_common";

static void register_free(void);
static void register_heap(void);
static void register_membench(void);
static void register_flash_stats(void);
static void register_version(void);
static void register_restart(void);
#if WITH_TASKS_INFO
static void register_tasks(void);
static void register_top(void);
#endif
static void register_log_level(void);

void register_system_common(void)
{
    register_free();
    register_heap();
    register_membench();
    register_flash_stats();
    register_version();
    register_restart();
#if WITH_TASKS_INFO
    register_tasks();
    register_top();
#endif
    register_log_level();
}


/* 'version' command */
static int get_version(int argc, char **argv)
{
    const char *model;
    esp_chip_info_t info;
    uint32_t flash_size;
    esp_chip_info(&info);

    switch(info.model) {
        case CHIP_ESP32:
            model = "ESP32";
            break;
        case CHIP_ESP32S2:
            model = "ESP32-S2";
            break;
        case CHIP_ESP32S3:
            model = "ESP32-S3";
            break;
        case CHIP_ESP32C3:
            model = "ESP32-C3";
            break;
        case CHIP_ESP32H2:
            model = "ESP32-H2";
            break;
        case CHIP_ESP32C2:
            model = "ESP32-C2";
            break;
        case CHIP_ESP32P4:
            model = "ESP32-P4";
            break;
        case CHIP_ESP32C5:
            model = "ESP32-C5";
            break;
        default:
            model = "Unknown";
            break;
    }

    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return 1;
    }
    /* The running firmware first: it is what `version` is usually asked for, and until now the
       one thing this command did not say. The version string is PROJECT_VER, which ESP-IDF takes
       from `git describe` unless the project sets it, so a release build reports its tag and a
       development build reports the commit it came from, with -dirty for uncommitted changes. */
    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha[17];
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    printf("App:%s %s\r\n", app->project_name, app->version);
    printf("\tbuilt:%s %s\r\n", app->date, app->time);
    printf("\tELF SHA-256:%s\r\n", elf_sha);
    printf("IDF Version:%s\r\n", esp_get_idf_version());
    printf("Chip info:\r\n");
    printf("\tmodel:%s\r\n", model);
    printf("\tcores:%d\r\n", info.cores);
    printf("\tfeature:%s%s%s%s%"PRIu32"%s\r\n",
           info.features & CHIP_FEATURE_WIFI_BGN ? "/802.11bgn" : "",
           info.features & CHIP_FEATURE_BLE ? "/BLE" : "",
           info.features & CHIP_FEATURE_BT ? "/BT" : "",
           info.features & CHIP_FEATURE_EMB_FLASH ? "/Embedded-Flash:" : "/External-Flash:",
           flash_size / (1024 * 1024), " MB");
    printf("\trevision number:%d\r\n", info.revision);
    return 0;
}

static void register_version(void)
{
    const esp_console_cmd_t cmd = {
        .command = "version",
        .help = "The running firmware's version and build, then the IDF version and the chip",
        .hint = NULL,
        .func = &get_version,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** 'restart' command restarts the program */

static int restart(int argc, char **argv)
{
    ESP_LOGI(TAG, "Restarting");
    esp_restart();
}

static void register_restart(void)
{
    const esp_console_cmd_t cmd = {
        .command = "restart",
        .help = "Software reset of the chip",
        .hint = NULL,
        .func = &restart,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** 'free' command prints available heap memory */

static int free_mem(int argc, char **argv)
{
    printf("%"PRIu32"\n", esp_get_free_heap_size());
    return 0;
}

static void register_free(void)
{
    const esp_console_cmd_t cmd = {
        .command = "free",
        .help = "Get the current size of free heap memory",
        .hint = NULL,
        .func = &free_mem,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'heap' command prints minimum heap size */
static int heap_size(int argc, char **argv)
{
    uint32_t heap_size = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    printf("min heap size: %"PRIu32"\n", heap_size);
    return 0;
}

static void register_heap(void)
{
    const esp_console_cmd_t heap_cmd = {
        .command = "heap",
        .help = "Get minimum size of free heap memory that was available during program execution",
        .hint = NULL,
        .func = &heap_size,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&heap_cmd) );

}

/*
 * 'membench': memory bandwidth as this build actually runs it. The one readout of the
 * PSRAM and flash clocks that does not need the boot log: 80 -> 120 MHz shows up here as
 * throughput. PSRAM->PSRAM is what a framebuffer copy or a PSRAM-resident draw buffer
 * pays; flash read goes through SPI1 (esp_flash_read), which is the flash clock alone.
 */
#define MB_PSRAM_BYTES (256 * 1024)
#define MB_CHUNK       (8 * 1024)

static double mb_per_s(size_t bytes, int64_t us)
{
    return us > 0 ? ((double)bytes / 1048576.0) / ((double)us / 1e6) : 0.0;
}

static int membench(int argc, char **argv)
{
    uint8_t *a = heap_caps_malloc(MB_PSRAM_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(MB_PSRAM_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *in = heap_caps_malloc(MB_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *in2 = heap_caps_malloc(MB_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a || !b || !in || !in2) {
        printf("membench: allocation failed (psram %s %s, internal %s %s; largest free internal block %u B)\n",
               a ? "ok" : "FAIL", b ? "ok" : "FAIL", in ? "ok" : "FAIL", in2 ? "ok" : "FAIL",
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        free(a); free(b); free(in); free(in2);
        return 1;
    }
    memset(a, 0x5a, MB_PSRAM_BYTES);
    const int passes = 4;
    int64_t t0, t1;

    t0 = esp_timer_get_time();
    for (int i = 0; i < passes; i++) memcpy(b, a, MB_PSRAM_BYTES);
    t1 = esp_timer_get_time();
    printf("psram -> psram memcpy   %6.1f MB/s  (%d x 256 KB)\n", mb_per_s((size_t)passes * MB_PSRAM_BYTES, t1 - t0), passes);

    t0 = esp_timer_get_time();
    for (int i = 0; i < passes; i++)
        for (size_t off = 0; off < MB_PSRAM_BYTES; off += MB_CHUNK) memcpy(in, a + off, MB_CHUNK);
    t1 = esp_timer_get_time();
    printf("psram -> internal read  %6.1f MB/s  (8 KB chunks, the bounce-buffer path)\n",
           mb_per_s((size_t)passes * MB_PSRAM_BYTES, t1 - t0));

    t0 = esp_timer_get_time();
    for (int i = 0; i < passes; i++)
        for (size_t off = 0; off < MB_PSRAM_BYTES; off += MB_CHUNK) memcpy(a + off, in, MB_CHUNK);
    t1 = esp_timer_get_time();
    printf("internal -> psram write %6.1f MB/s\n", mb_per_s((size_t)passes * MB_PSRAM_BYTES, t1 - t0));

    t0 = esp_timer_get_time();
    for (int i = 0; i < 64; i++) memcpy(in2, in, MB_CHUNK);
    t1 = esp_timer_get_time();
    printf("internal -> internal    %6.1f MB/s  (reference)\n", mb_per_s(64 * MB_CHUNK, t1 - t0));

    /* SPI1 flash read into internal RAM: the app partition's first 512 KB, chunked. */
    const size_t flash_bytes = 512 * 1024;
    t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < flash_bytes && err == ESP_OK; off += MB_CHUNK)
        err = esp_flash_read(NULL, in, 0x20000 + off, MB_CHUNK);
    t1 = esp_timer_get_time();
    if (err == ESP_OK) printf("flash -> internal read  %6.1f MB/s  (esp_flash_read, 512 KB)\n", mb_per_s(flash_bytes, t1 - t0));
    else printf("flash read failed: %s\n", esp_err_to_name(err));

    free(a); free(b); free(in); free(in2);
    return 0;
}

static void register_membench(void)
{
    const esp_console_cmd_t cmd = {
        .command = "membench",
        .help = "Measure PSRAM and flash bandwidth: the readout of their clocks that needs no boot log",
        .hint = NULL,
        .func = &membench,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/*
 * 'flash-stats': how often this firmware writes and erases flash, and for how long.
 * Every flash program or erase runs with interrupts off on its core and the other core
 * halted, so nothing time-critical (a display refill ISR, say) is serviced for its
 * duration: a page program is ~1 ms and a sector erase tens of ms. These counters
 * (CONFIG_SPI_FLASH_ENABLE_COUNTERS) say who is doing that and when.
 */
static int flash_stats(int argc, char **argv)
{
#if CONFIG_SPI_FLASH_ENABLE_COUNTERS
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        esp_flash_reset_counters();
        printf("flash counters reset\n");
        return 0;
    }
    const esp_flash_counters_t *c = esp_flash_get_counters();
    printf("%-6s %8s %10s %12s %9s\n", "op", "count", "bytes", "total us", "avg us");
    const struct { const char *name; const esp_flash_counter_t *ctr; } rows[] = {
        { "read", &c->read }, { "write", &c->write }, { "erase", &c->erase },
    };
    for (size_t i = 0; i < 3; i++) {
        printf("%-6s %8" PRIu32 " %10" PRIu32 " %12" PRIu32 " %9" PRIu32 "\n", rows[i].name,
               rows[i].ctr->count, rows[i].ctr->bytes, rows[i].ctr->time,
               rows[i].ctr->count ? rows[i].ctr->time / rows[i].ctr->count : 0);
    }
    return 0;
#else
    (void)argc; (void)argv;
    printf("built without CONFIG_SPI_FLASH_ENABLE_COUNTERS\n");
    return 1;
#endif
}

static void register_flash_stats(void)
{
    const esp_console_cmd_t cmd = {
        .command = "flash-stats",
        .help = "Flash read/write/erase counts and time since boot or the last `flash-stats reset`",
        .hint = "[reset]",
        .func = &flash_stats,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** 'tasks' command prints the list of tasks and related information */
#if WITH_TASKS_INFO

static int tasks_info(int argc, char **argv)
{
    const size_t bytes_per_task = 40; /* see vTaskList description */
    char *task_list_buffer = malloc(uxTaskGetNumberOfTasks() * bytes_per_task);
    if (task_list_buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate buffer for vTaskList output");
        return 1;
    }
    fputs("Task Name\tStatus\tPrio\tHWM\tTask#", stdout);
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    fputs("\tAffinity", stdout);
#endif
    fputs("\n", stdout);
    vTaskList(task_list_buffer);
    fputs(task_list_buffer, stdout);
    free(task_list_buffer);
    return 0;
}

static void register_tasks(void)
{
    const esp_console_cmd_t cmd = {
        .command = "tasks",
        .help = "Get information about running tasks",
        .hint = NULL,
        .func = &tasks_info,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'top': per-task CPU share over a sampling window.
 *
 * `tasks` shows state and stack headroom but not time. This walks uxTaskGetSystemState()
 * twice, `top [secs]` apart (default 2), and prints each task's share of the interval,
 * sorted. The denominator is wall-clock time (ulTotalRunTime is the esp_timer count, and
 * is deliberately NOT divided by the core count), so a task that owns a whole core reads
 * 100% and the two IDLE rows are the free time on each core.
 *
 * Found necessary on r2_domeplayer when a priority-5 task on core 1 sat above the UI task
 * at priority 4 on the same core and starved it silently: the screen froze, and nothing on
 * the console could say why. */
#define TOP_MAX_TASKS 40

static int top_cmd(int argc, char **argv)
{
    int secs = 2;
    if (argc > 1) {
        secs = atoi(argv[1]);
        if (secs < 1 || secs > 60) {
            printf("usage: top [1..60 seconds]\n");
            return 1;
        }
    }

    TaskStatus_t *before = calloc(TOP_MAX_TASKS, sizeof(TaskStatus_t));
    TaskStatus_t *after = calloc(TOP_MAX_TASKS, sizeof(TaskStatus_t));
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        printf("no memory\n");
        return 1;
    }

    configRUN_TIME_COUNTER_TYPE t0 = 0, t1 = 0;
    UBaseType_t n0 = uxTaskGetSystemState(before, TOP_MAX_TASKS, &t0);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    UBaseType_t n1 = uxTaskGetSystemState(after, TOP_MAX_TASKS, &t1);

    uint64_t total = (uint64_t)(t1 - t0);
    if (total == 0) {
        total = 1;
    }

    /* Match by handle; a task created inside the window has no baseline and is skipped. */
    uint64_t delta[TOP_MAX_TASKS];
    int order[TOP_MAX_TASKS];
    int count = 0;
    for (UBaseType_t i = 0; i < n1; i++) {
        for (UBaseType_t j = 0; j < n0; j++) {
            if (after[i].xHandle == before[j].xHandle) {
                delta[count] = (uint64_t)(after[i].ulRunTimeCounter - before[j].ulRunTimeCounter);
                order[count] = (int)i;
                count++;
                break;
            }
        }
    }
    for (int i = 1; i < count; i++) {
        uint64_t d = delta[i];
        int o = order[i];
        int j = i;
        while (j > 0 && delta[j - 1] < d) {
            delta[j] = delta[j - 1];
            order[j] = order[j - 1];
            j--;
        }
        delta[j] = d;
        order[j] = o;
    }

    /* prio is the running priority, base the one the task was created with; they differ
     * while a mutex the task holds has lent it a waiter's priority. */
    printf("%-16s %6s %5s %5s  (window %d s, %" PRIu64 " us per core)\n", "task", "cpu%", "prio",
           "base", secs, total);
    for (int i = 0; i < count; i++) {
        const TaskStatus_t *t = &after[order[i]];
        unsigned tenths = (unsigned)((delta[i] * 1000) / total);
        printf("%-16s %3u.%1u%% %5u %5u\n", t->pcTaskName, tenths / 10, tenths % 10,
               (unsigned)t->uxCurrentPriority, (unsigned)t->uxBasePriority);
    }

    free(before);
    free(after);
    return 0;
}

static void register_top(void)
{
    const esp_console_cmd_t cmd = {
        .command = "top",
        .help = "Per-task CPU share over a window: top [seconds]",
        .hint = NULL,
        .func = &top_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

#endif // WITH_TASKS_INFO

/** log_level command changes log level via esp_log_level_set */

static struct {
    struct arg_str *tag;
    struct arg_str *level;
    struct arg_end *end;
} log_level_args;

static const char* s_log_level_names[] = {
    "none",
    "error",
    "warn",
    "info",
    "debug",
    "verbose"
};

static int log_level(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &log_level_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, log_level_args.end, argv[0]);
        return 1;
    }
    assert(log_level_args.tag->count == 1);
    assert(log_level_args.level->count == 1);
    const char* tag = log_level_args.tag->sval[0];
    const char* level_str = log_level_args.level->sval[0];
    esp_log_level_t level;
    size_t level_len = strlen(level_str);
    for (level = ESP_LOG_NONE; level <= ESP_LOG_VERBOSE; level++) {
        if (memcmp(level_str, s_log_level_names[level], level_len) == 0) {
            break;
        }
    }
    if (level > ESP_LOG_VERBOSE) {
        printf("Invalid log level '%s', choose from none|error|warn|info|debug|verbose\n", level_str);
        return 1;
    }
    if (level > CONFIG_LOG_MAXIMUM_LEVEL) {
        printf("Can't set log level to %s, max level limited in menuconfig to %s. "
               "Please increase CONFIG_LOG_MAXIMUM_LEVEL in menuconfig.\n",
               s_log_level_names[level], s_log_level_names[CONFIG_LOG_MAXIMUM_LEVEL]);
        return 1;
    }
    esp_log_level_set(tag, level);
    return 0;
}

static void register_log_level(void)
{
    log_level_args.tag = arg_str1(NULL, NULL, "<tag|*>", "Log tag to set the level for, or * to set for all tags");
    log_level_args.level = arg_str1(NULL, NULL, "<none|error|warn|info|debug|verbose>", "Log level to set. Abbreviated words are accepted.");
    log_level_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "log_level",
        .help = "Set log level for all tags or a specific tag.",
        .hint = NULL,
        .func = &log_level,
        .argtable = &log_level_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/*
 * 'gpio': read or drive one pin from the console, for checking bench wiring before the
 * firmware that will own the pin exists. `set` makes the pin an input-and-output and drives
 * it, so the readback in the reply is the pad itself; `release` puts it back to a floating
 * input, so a driver that claims it later finds it untouched. The application says which
 * pins must never be driven -- display and bus pins, strapping pins, flash and PSRAM --
 * and why; `get` reads those without reconfiguring them.
 */
static const gpio_reserved_t *s_gpio_reserved;
static size_t s_gpio_reserved_count;
static uint64_t s_gpio_driven;   /* pins `set` has configured, so `get` leaves them alone */

static const char *gpio_reserved_why(int pin)
{
    for (size_t i = 0; i < s_gpio_reserved_count; i++) {
        if (s_gpio_reserved[i].pin == pin) {
            return s_gpio_reserved[i].why ? s_gpio_reserved[i].why : "reserved";
        }
    }
    return NULL;
}

static int cmd_gpio(int argc, char **argv)
{
    static const char *usage = "usage: gpio get <n> | set <n> 0|1 | release <n>\n";
    if (argc < 3) {
        printf("%s", usage);
        return 1;
    }
    char *end = NULL;
    long pin = strtol(argv[2], &end, 0);
    if (end == argv[2] || *end != '\0' || pin < 0 || pin >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(pin)) {
        printf("not a GPIO: %s\n", argv[2]);
        return 1;
    }
    const char *why = gpio_reserved_why((int)pin);
    const uint64_t bit = 1ULL << pin;

    if (strcmp(argv[1], "get") == 0 && argc == 3) {
        if (why == NULL && (s_gpio_driven & bit) == 0) {
            const gpio_config_t io = {
                .pin_bit_mask = bit,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
        }
        printf("gpio %ld = %d%s%s%s\n", pin, gpio_get_level((gpio_num_t)pin),
               why ? " (reserved: " : "", why ? why : "", why ? ", not reconfigured)" : "");
        return 0;
    }
    if (why != NULL) {
        printf("gpio %ld is reserved: %s\n", pin, why);
        return 1;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        if (strcmp(argv[3], "0") != 0 && strcmp(argv[3], "1") != 0) {
            printf("%s", usage);
            return 1;
        }
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            printf("gpio %ld is input-only\n", pin);
            return 1;
        }
        const int level = argv[3][0] - '0';
        const gpio_config_t io = {
            .pin_bit_mask = bit,
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        /* Configure once: a second gpio_config() on a pin logs a "conflict" warning. */
        esp_err_t err = (s_gpio_driven & bit) ? ESP_OK : gpio_config(&io);
        if (err == ESP_OK) {
            err = gpio_set_level((gpio_num_t)pin, level);
        }
        if (err != ESP_OK) {
            printf("gpio %ld: %s\n", pin, esp_err_to_name(err));
            return 1;
        }
        s_gpio_driven |= bit;
        printf("gpio %ld driven %d, pad reads %d\n", pin, level, gpio_get_level((gpio_num_t)pin));
        return 0;
    }
    if (strcmp(argv[1], "release") == 0 && argc == 3) {
        /* gpio_reset_pin() leaves the pull-up on, which is not "untouched" for a pin about to
         * carry a single-wire bus; float it. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin((gpio_num_t)pin));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING));
        s_gpio_driven &= ~bit;
        printf("gpio %ld released: floating input\n", pin);
        return 0;
    }
    printf("%s", usage);
    return 1;
}

void register_gpio(const gpio_reserved_t *reserved, size_t count)
{
    s_gpio_reserved = reserved;
    s_gpio_reserved_count = count;
    const esp_console_cmd_t cmd = {
        .command = "gpio",
        .help = "Read or drive one GPIO, for bench wiring checks. set drives an output (the reply "
                "reads the pad back); release floats it again. Pins the board owns are refused.",
        .hint = "get <n> | set <n> 0|1 | release <n>",
        .func = &cmd_gpio,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}
