/*
 * `ota`: application updates from the console. See cmd_ota.h.
 *
 * The update itself is ota_core's, shared with the web server: `ota put` streams an image
 * over XMODEM-1K into the slot that is not running, `ota pull` has whoever installed a puller
 * fetch one, and only once the whole image has verified -- esp_ota_end() checks its header,
 * chip and SHA-256 -- does it become the boot image. Anything short of that leaves the running
 * image booting. An image that is not an application at all is refused on its first block.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "cmd_ota.h"
#include "ota_core.h"
#include "xfer_session.h"
#include "xmodem.h"

static const char *TAG = "ota";
static int s_uart = -1;

#define MAX_SLOTS 4

/* ota: each application slot, what is in it, and which is running and which boots. */
static int ota_status(void)
{
    ota_core_slot_t slots[MAX_SLOTS];
    const size_t n = ota_core_slots(slots, MAX_SLOTS);
    for (size_t i = 0; i < n; i++) {
        const ota_core_slot_t *s = &slots[i];
        const char *role = s->running && s->boots ? "running, boots"
                         : s->running              ? "running"
                         : s->boots                ? "boots next"
                         : s->next                 ? "next update"
                         : "";
        printf("%-8s 0x%06" PRIx32 " %5" PRIu32 " KB  %-14s", s->label, s->address, s->size / 1024, role);
        if (s->have_app) {
            printf("  %s %s, built %s %s", s->app.project_name, s->app.version, s->app.date, s->app.time);
        } else {
            printf("  empty");
        }
        if (s->have_state) {
            printf(", %s", ota_core_img_state_name(s->state));
        }
        printf("\n");
    }
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    printf("rollback on: an updated image runs on trial until it is up and confirms itself; "
           "a reset before then boots the image before it\n");
#else
    printf("rollback off (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)\n");
#endif
    return 0;
}

static esp_err_t ota_sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return ota_core_write(data, len);
}

/* A staged image: make it the boot image and, unless told not to, restart into it. */
static int boot_staged(bool restart)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ota_core_session_t s;
    ota_core_get(&s);
    const esp_err_t err = ota_core_activate(NULL);
    if (err != ESP_OK) {
        printf("ota: cannot make %s the boot image: %s; %s still boots\n", s.slot, esp_err_to_name(err),
               running->label);
        return 1;
    }
    printf("ota: %s now boots %s %s, built %s %s\n", s.slot, s.app.project_name, s.app.version, s.app.date,
           s.app.time);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    printf("ota: on trial until it is up; a reset before then boots %s again\n", running->label);
#endif
    if (!restart) {
        printf("ota: restart to run it\n");
        return 0;
    }
    printf("ota: restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static const char *begin_err(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE: return "another update is in progress";
    case ESP_ERR_NOT_FOUND:     return "no slot to update: the partition table needs two OTA app partitions";
    case ESP_ERR_INVALID_SIZE:  return "it will not fit the slot";
    default:                    return esp_err_to_name(err);
    }
}

/* ota put [-b baud] [-n] [-d] <size> */
static int ota_put(int argc, char **argv)
{
    uint32_t baud = 0;
    bool restart = true;
    bool dry = false;
    size_t size = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-n") == 0) {
            restart = false;
        } else if (strcmp(argv[i], "-d") == 0) {
            dry = true;
        } else if (argv[i][0] != '-' && size == 0) {
            size = (size_t)strtoul(argv[i], NULL, 10);
        } else {
            size = 0;
            break;
        }
    }
    if (size == 0) {
        printf("usage: ota put [-b baud] [-n] [-d] <size>\n"
               "  -n: make it the boot image, but do not restart\n"
               "  -d: a link test -- receive and hash it, write nothing\n");
        return 1;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part != NULL && size > part->size) {
        printf("ota: %u bytes will not fit the %" PRIu32 " KB slot %s\n", (unsigned)size,
               part->size / 1024, part->label);
        return 1;
    }
    if (!dry && part != NULL) {
        /* Erase first, while the host waits for the ready line: a few seconds for a megabyte. */
        printf("ota: erasing %s for %u bytes\n", part->label, (unsigned)size);
        fflush(stdout);
    }
    esp_err_t err = ota_core_begin(OTA_CORE_SRC_CONSOLE, size, NULL,
                                   OTA_CORE_ERASE_FIRST | (dry ? OTA_CORE_DRY_RUN : 0));
    if (err != ESP_OK) {
        ota_core_session_t s;
        ota_core_get(&s);
        printf("ota: cannot start the update: %s\n", s.error[0] ? s.error : begin_err(err));
        return 1;
    }

    printf("xmodem: ready to receive %u bytes at %" PRIu32 " baud: %s\n", (unsigned)size,
           baud ? baud : xfer_console_baud(s_uart), part->label);
    xfer_session_t session;
    err = xfer_session_begin(&session, s_uart, baud);
    xmodem_stats_t stats = { 0 };
    const int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) {
        const xmodem_config_t xcfg = { .expected_size = size };
        err = xmodem_receive(&session.io, &xcfg, ota_sink, NULL, &stats);
        xfer_session_end(&session);
    }
    const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
    if (stats.retries) {
        printf("xmodem: %u retries: %u blocks timed out, %u bad, %u never arrived "
               "(%u stray bytes skipped)\n", stats.retries, stats.timeouts, stats.bad_blocks,
               stats.retries - stats.timeouts - stats.bad_blocks, stats.skipped);
    }

    ota_core_session_t s;
    if (err != ESP_OK) {
        ota_core_get(&s);
        if (s.state != OTA_CORE_FAILED) {
            ota_core_fail("%s", xfer_err(err));
            ota_core_get(&s);
        }
        printf("ota: failed: %s, after %u bytes; %s still boots\n", s.error[0] ? s.error : xfer_err(err),
               (unsigned)stats.bytes, running->label);
        return 1;
    }
    printf("received %u bytes in %.1f s (%.1f KB/s, %u retries)\n", (unsigned)stats.bytes, secs,
           secs > 0 ? (double)stats.bytes / 1024.0 / secs : 0.0, stats.retries);

    err = ota_core_finish(NULL);
    ota_core_get(&s);
    if (s.sha256[0]) {
        printf("sha256 %s  %s\n", s.sha256, part->label);
    }
    if (dry) {
        printf("ota: dry run: nothing written; %s still boots\n", running->label);
        return 0;
    }
    if (err != ESP_OK) {
        printf("ota: %s; %s still boots\n", s.error, running->label);
        return 1;
    }
    return boot_staged(restart);
}

/* ota pull [-n] [-s] [-f] [--sha256 <hex>] <url|channel> */
static int ota_pull(int argc, char **argv)
{
    bool restart = true;
    bool stage_only = false;
    bool any_project = false;
    const char *what = NULL;
    const char *sha = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            restart = false;
        } else if (strcmp(argv[i], "-s") == 0) {
            stage_only = true;
        } else if (strcmp(argv[i], "-f") == 0) {
            any_project = true;
        } else if (strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) {
            sha = argv[++i];
        } else if (argv[i][0] != '-' && what == NULL) {
            what = argv[i];
        } else {
            what = NULL;
            break;
        }
    }
    if (what == NULL) {
        printf("usage: ota pull [-n] [-s] [-f] [--sha256 <hex>] <url|channel>\n"
               "  <url>: an application image, or a release manifest (manifest.json)\n"
               "  <channel>: a release channel, such as `latest`\n"
               "  -n: make it the boot image, but do not restart\n"
               "  -s: stage it only; `ota activate` makes it the boot image\n"
               "  -f: accept an image built for another project\n");
        return 1;
    }
    const ota_core_pull_fn pull = ota_core_puller();
    if (pull == NULL) {
        printf("ota: this firmware cannot pull images (no HTTP client)\n");
        return 1;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const ota_core_pull_req_t req = { .what = what, .sha256 = sha, .any_project = any_project };
    char why[128] = "";
    esp_err_t err = pull(&req, why, sizeof(why));
    if (err != ESP_OK) {
        printf("ota: %s\n", why[0] ? why : begin_err(err));
        return 1;
    }

    /* The pull runs on its own task; report on it until it ends. */
    ota_core_session_t s;
    int shown = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        ota_core_get(&s);
        if (s.state == OTA_CORE_RECEIVING && s.bytes > 0) {
            if (shown < 0) {
                printf("ota: downloading %s into %s\n", s.url, s.slot);
                shown = 0;
            }
            const int pct = s.total ? (int)(s.bytes * 100 / s.total) : -1;
            if (pct >= shown + 10) {
                shown = pct - pct % 10;
                printf("ota: %d%% (%u of %u bytes)\n", shown, (unsigned)s.bytes, (unsigned)s.total);
            }
        }
        if (s.state != OTA_CORE_RECEIVING && s.state != OTA_CORE_VERIFYING) {
            break;
        }
    }
    if (s.state != OTA_CORE_STAGED) {
        printf("ota: pull failed: %s; %s still boots\n", s.error[0] ? s.error : ota_core_state_name(s.state),
               running->label);
        return 1;
    }
    const double secs = (double)(s.finished_us - s.started_us) / 1e6;
    printf("received %u bytes in %.1f s\nsha256 %s  %s\n", (unsigned)s.bytes, secs, s.sha256, s.slot);
    if (stage_only) {
        printf("ota: %s %s staged in %s; `ota activate` makes it the boot image\n", s.app.project_name,
               s.app.version, s.slot);
        return 0;
    }
    return boot_staged(restart);
}

/* ota activate [-n] [slot] */
static int ota_activate(int argc, char **argv)
{
    bool restart = true;
    const char *slot = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            restart = false;
        } else if (argv[i][0] != '-' && slot == NULL) {
            slot = argv[i];
        } else {
            printf("usage: ota activate [-n] [slot]\n"
                   "  alone: the image an update staged; with a slot, the image already in it\n"
                   "  -n: make it the boot image, but do not restart\n");
            return 1;
        }
    }
    if (slot == NULL) {
        ota_core_session_t s;
        ota_core_get(&s);
        if (s.state != OTA_CORE_STAGED) {
            printf("ota: nothing staged; name a slot (see `ota`)\n");
            return 1;
        }
        return boot_staged(restart);
    }
    const esp_err_t err = ota_core_activate(slot);
    if (err != ESP_OK) {
        printf("ota: cannot boot %s: %s\n", slot,
               err == ESP_ERR_NOT_FOUND ? "no such slot"
               : err == ESP_ERR_INVALID_STATE ? "an update is writing it"
               : err == ESP_ERR_OTA_VALIDATE_FAILED ? "it holds no good image"
               : esp_err_to_name(err));
        return 1;
    }
    ota_core_session_t s;
    ota_core_get(&s);
    printf("ota: %s now boots %s %s\n", s.slot, s.app.project_name, s.app.version);
    if (!restart) {
        printf("ota: restart to run it\n");
        return 0;
    }
    printf("ota: restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int ota_cmd(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        return ota_status();
    }
    if (strcmp(argv[1], "put") == 0) {
        return ota_put(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "pull") == 0) {
        return ota_pull(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "activate") == 0) {
        return ota_activate(argc - 2, argv + 2);
    }
    printf("usage: ota [status] | ota put [-b baud] [-n] [-d] <size> | ota pull [-n] [-s] [-f] <url|channel>"
           " | ota activate [-n] [slot]\n");
    return 1;
}

esp_err_t register_ota(int uart_num)
{
    s_uart = uart_num;
    const esp_console_cmd_t cmd = {
        .command = "ota",
        .help = "Application updates: each slot's image and state; `put` a new image over XMODEM-1K "
                "(host side: esp-console-kit/tools/fs_xfer.py ota), `pull` one from a URL or release "
                "channel, or `activate` a slot",
        .hint = "[status] | put [-b baud] [-n] [-d] <size> | pull [-n] [-s] [-f] <url|channel> | activate [-n] [slot]",
        .func = ota_cmd,
    };
    return esp_console_cmd_register(&cmd);
}

void ota_confirm_running(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s confirmed: the update stays", run->label);
    } else {
        ESP_LOGE(TAG, "%s: cannot confirm the update: %s", run->label, esp_err_to_name(err));
    }
}
