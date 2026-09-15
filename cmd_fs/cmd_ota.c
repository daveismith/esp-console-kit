/*
 * `ota`: application updates over the console with XMODEM-1K. See cmd_ota.h.
 *
 * `ota put` erases the slot that is not running, streams the image into it as the blocks
 * arrive, and only once the whole image has verified -- esp_ota_end() checks its header,
 * chip and SHA-256 -- makes it the boot image. Anything short of that leaves the running
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
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "cmd_ota.h"
#include "xfer_session.h"
#include "xmodem.h"

static const char *TAG = "ota";
static int s_uart = -1;

static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new, not yet booted";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on trial, not yet confirmed";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "rolled back";
    default:                         return "no OTA record";
    }
}

/* ota: each application slot, what is in it, and which is running and which boots. */
static int ota_status(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        const char *role = p == run && p == boot ? "running, boots"
                         : p == run              ? "running"
                         : p == boot             ? "boots next"
                         : p == next             ? "next update"
                         : "";
        printf("%-8s 0x%06" PRIx32 " %5" PRIu32 " KB  %-14s", p->label, p->address, p->size / 1024, role);
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
            printf("  %s %s, built %s %s", d.project_name, d.version, d.date, d.time);
        } else {
            printf("  empty");
        }
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
            printf(", %s", state_name(st));
        }
        printf("\n");
        it = esp_partition_next(it);
    }
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    printf("rollback on: an updated image runs on trial until it is up and confirms itself; "
           "a reset before then boots the image before it\n");
#else
    printf("rollback off (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)\n");
#endif
    return 0;
}

typedef struct {
    esp_ota_handle_t handle;
    psa_hash_operation_t sha;
    bool dry;                   /* -d: receive and hash, write nothing */
} ota_ctx_t;

static esp_err_t ota_sink(void *ctx, const uint8_t *data, size_t len)
{
    ota_ctx_t *o = ctx;
    /* The first write checks the image's magic byte: not an application, and the transfer
     * is cancelled. */
    const esp_err_t err = o->dry ? ESP_OK : esp_ota_write(o->handle, data, len);
    if (err == ESP_OK) {
        psa_hash_update(&o->sha, data, len);
    }
    return err;
}

static bool sha_begin(psa_hash_operation_t *op)
{
    *op = psa_hash_operation_init();
    return psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(op, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

static bool sha_end(psa_hash_operation_t *op, char hex[65])
{
    uint8_t digest[32];
    size_t n = 0;
    if (psa_hash_finish(op, digest, sizeof(digest), &n) != PSA_SUCCESS || n != sizeof(digest)) {
        psa_hash_abort(op);
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    return true;
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
    if (part == NULL) {
        printf("ota: no slot to update: the partition table needs two OTA app partitions\n");
        return 1;
    }
    if (size > part->size) {
        printf("ota: %u bytes will not fit the %" PRIu32 " KB slot %s\n", (unsigned)size,
               part->size / 1024, part->label);
        return 1;
    }

    ota_ctx_t ctx = { .dry = dry };
    esp_err_t err = ESP_OK;
    if (!dry) {
        /* Erase first, while the host waits for the ready line: a few seconds for a megabyte. */
        printf("ota: erasing %s for %u bytes\n", part->label, (unsigned)size);
        fflush(stdout);
        err = esp_ota_begin(part, size, &ctx.handle);
        if (err != ESP_OK) {
            printf("ota: cannot start the update: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    if (!sha_begin(&ctx.sha)) {
        if (!dry) {
            esp_ota_abort(ctx.handle);
        }
        printf("ota: no SHA engine\n");
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
        err = xmodem_receive(&session.io, &xcfg, ota_sink, &ctx, &stats);
        xfer_session_end(&session);
    }
    const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
    char hex[65] = "";
    const bool hashed = sha_end(&ctx.sha, hex);
    if (stats.retries) {
        printf("xmodem: %u retries: %u blocks timed out, %u bad, %u never arrived "
               "(%u stray bytes skipped)\n", stats.retries, stats.timeouts, stats.bad_blocks,
               stats.retries - stats.timeouts - stats.bad_blocks, stats.skipped);
    }

    if (err != ESP_OK) {
        if (!dry) {
            esp_ota_abort(ctx.handle);
        }
        printf("ota: failed: %s, after %u bytes; %s still boots\n", xfer_err(err),
               (unsigned)stats.bytes, running->label);
        return 1;
    }
    printf("received %u bytes in %.1f s (%.1f KB/s, %u retries)\n", (unsigned)stats.bytes, secs,
           secs > 0 ? (double)stats.bytes / 1024.0 / secs : 0.0, stats.retries);
    if (hashed) {
        printf("sha256 %s  %s\n", hex, part->label);
    }
    if (dry) {
        printf("ota: dry run: nothing written; %s still boots\n", running->label);
        return 0;
    }

    err = esp_ota_end(ctx.handle);
    if (err != ESP_OK) {
        printf("ota: the image did not verify (%s); %s still boots\n", esp_err_to_name(err),
               running->label);
        return 1;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        printf("ota: cannot make %s the boot image: %s; %s still boots\n", part->label,
               esp_err_to_name(err), running->label);
        return 1;
    }
    esp_app_desc_t d = { 0 };
    esp_ota_get_partition_description(part, &d);
    printf("ota: %s now boots %s %s, built %s %s\n", part->label, d.project_name, d.version, d.date,
           d.time);
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

static int ota_cmd(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        return ota_status();
    }
    if (strcmp(argv[1], "put") == 0) {
        return ota_put(argc - 2, argv + 2);
    }
    printf("usage: ota [status] | ota put [-b baud] [-n] [-d] <size>\n");
    return 1;
}

esp_err_t register_ota(int uart_num)
{
    s_uart = uart_num;
    const esp_console_cmd_t cmd = {
        .command = "ota",
        .help = "Application updates: each slot's image and state, or `put` a new image over "
                "XMODEM-1K into the slot not running (host side: esp-console-kit/tools/fs_xfer.py ota)",
        .hint = "[status] | put [-b baud] [-n] [-d] <size>",
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
