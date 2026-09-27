/*
 * ota_core: the update session every transport shares. See ota_core.h.
 *
 * The session is written into the slot that is not running, so nothing that happens during
 * it -- a dropped connection, a truncated or foreign image, a power cut -- can leave the board
 * without a working image to boot. Only a verified image is ever made the boot image.
 *
 * Locking: `s_lock` guards the session record, which anyone may read. The esp_ota handle and
 * the hash belong to the session's owner, the one task that called begin, and only it touches
 * them; others can only raise `s_cancel`, which the owner's next write sees.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "ota_core.h"

static const char *TAG = "ota";

/* An image's first bytes: its header, the first segment's header, then the app descriptor. */
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static SemaphoreHandle_t s_lock;
static ota_core_session_t s_session;
static volatile bool s_cancel;

/* The owner's */
static const esp_partition_t *s_part;
static esp_ota_handle_t s_handle;
static bool s_open;                 /* s_handle needs an end or an abort */
static psa_hash_operation_t s_sha;
static bool s_hashing;
static unsigned s_flags;
static uint8_t s_head[HEAD_LEN];
static bool s_head_checked;

static ota_core_hook_t s_begin_hook;
static void *s_begin_hook_ctx;
static ota_core_pull_fn s_puller;
static esp_timer_handle_t s_restart_timer;

static void lock(void)
{
    if (s_lock == NULL) {
        /* Whoever asks first -- the console, the web server's task -- creates it. */
        static StaticSemaphore_t buf;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        taskENTER_CRITICAL(&mux);
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateMutexStatic(&buf);
        }
        taskEXIT_CRITICAL(&mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static bool in_progress(ota_core_state_t st)
{
    return st == OTA_CORE_RECEIVING || st == OTA_CORE_VERIFYING;
}

const char *ota_core_state_name(ota_core_state_t state)
{
    switch (state) {
    case OTA_CORE_IDLE:      return "idle";
    case OTA_CORE_RECEIVING: return "receiving";
    case OTA_CORE_VERIFYING: return "verifying";
    case OTA_CORE_STAGED:    return "staged";
    case OTA_CORE_ACTIVE:    return "active";
    case OTA_CORE_FAILED:    return "failed";
    }
    return "?";
}

const char *ota_core_source_name(ota_core_source_t source)
{
    switch (source) {
    case OTA_CORE_SRC_CONSOLE: return "console";
    case OTA_CORE_SRC_UPLOAD:  return "upload";
    case OTA_CORE_SRC_PULL:    return "pull";
    default:                   return "none";
    }
}

const char *ota_core_img_state_name(int state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new, not yet booted";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on trial, not yet confirmed";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "rolled back";
    default:                         return "no OTA record";
    }
}

const char *ota_core_img_state_key(int state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "trial";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "rolled_back";
    default:                         return "none";
    }
}

void ota_core_set_begin_hook(ota_core_hook_t hook, void *ctx)
{
    s_begin_hook_ctx = ctx;
    s_begin_hook = hook;
}

void ota_core_set_puller(ota_core_pull_fn fn)
{
    s_puller = fn;
}

ota_core_pull_fn ota_core_puller(void)
{
    return s_puller;
}

void ota_core_get(ota_core_session_t *out)
{
    lock();
    *out = s_session;
    unlock();
}

bool ota_core_busy(void)
{
    lock();
    const bool busy = in_progress(s_session.state);
    unlock();
    return busy;
}

size_t ota_core_slots(ota_core_slot_t *out, size_t max)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    size_t n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL && n < max; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        ota_core_slot_t *s = &out[n++];
        memset(s, 0, sizeof(*s));
        strlcpy(s->label, p->label, sizeof(s->label));
        s->address = p->address;
        s->size = p->size;
        s->running = p == run;
        s->boots = p == boot;
        s->next = p == next;
        s->have_app = esp_ota_get_partition_description(p, &s->app) == ESP_OK;
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
            s->have_state = true;
            s->state = st;
        }
    }
    esp_partition_iterator_release(it);
    return n;
}

/* ------------------------------------------------------------------ a session */

static void set_failed_locked(const char *why)
{
    s_session.state = OTA_CORE_FAILED;
    strlcpy(s_session.error, why, sizeof(s_session.error));
    s_session.finished_us = esp_timer_get_time();
}

/* The owner's cleanup after a failure: whatever is open is closed. */
static void release(void)
{
    if (s_open) {
        esp_ota_abort(s_handle);
        s_open = false;
    }
    if (s_hashing) {
        psa_hash_abort(&s_sha);
        s_hashing = false;
    }
}

esp_err_t ota_core_begin(ota_core_source_t source, size_t size, const char *url, unsigned flags)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (size > part->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    lock();
    if (in_progress(s_session.state)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = OTA_CORE_RECEIVING;
    s_session.source = source;
    s_session.dry_run = (flags & OTA_CORE_DRY_RUN) != 0;
    strlcpy(s_session.slot, part->label, sizeof(s_session.slot));
    if (url != NULL) {
        strlcpy(s_session.url, url, sizeof(s_session.url));
    }
    s_session.total = size;
    s_session.started_us = esp_timer_get_time();
    s_cancel = false;
    unlock();

    s_part = part;
    s_flags = flags;
    s_head_checked = false;
    s_open = false;
    s_hashing = false;

    s_sha = psa_hash_operation_init();
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&s_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        lock();
        set_failed_locked("no SHA-256 engine");
        unlock();
        return ESP_ERR_NO_MEM;
    }
    s_hashing = true;

    if (!s_session.dry_run) {
        if (s_begin_hook != NULL) {
            s_begin_hook(s_begin_hook_ctx);
        }
        /* Erasing up front takes a few seconds a megabyte, and suits a sender that waits for a
         * ready line. As-written erasing suits one that just starts sending. */
        const size_t image_size = (flags & OTA_CORE_ERASE_FIRST) ? (size ? size : OTA_SIZE_UNKNOWN)
                                                                 : OTA_WITH_SEQUENTIAL_WRITES;
        const esp_err_t err = esp_ota_begin(part, image_size, &s_handle);
        if (err != ESP_OK) {
            char why[96];
            snprintf(why, sizeof(why), "cannot start the update: %s", esp_err_to_name(err));
            release();
            lock();
            set_failed_locked(why);
            unlock();
            return err;
        }
        s_open = true;
    }
    ESP_LOGI(TAG, "%s update into %s, %u bytes%s", ota_core_source_name(source), part->label,
             (unsigned)size, s_session.dry_run ? " (dry run)" : "");
    return ESP_OK;
}

void ota_core_set_source(const char *url, size_t total)
{
    lock();
    if (url != NULL) {
        strlcpy(s_session.url, url, sizeof(s_session.url));
    }
    s_session.total = total;
    unlock();
}

/* The first bytes are enough to know whether this is an application for this chip and this
 * project; saying so now saves sending the other megabyte. */
static esp_err_t check_head(char *why, size_t why_len)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)s_head;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(why, why_len, "not an application image (first byte 0x%02x, not 0x%02x)", hdr->magic,
                 ESP_IMAGE_HEADER_MAGIC);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        snprintf(why, why_len, "built for another chip (id %u, this is %u)", (unsigned)hdr->chip_id,
                 (unsigned)CONFIG_IDF_FIRMWARE_CHIP_ID);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    esp_app_desc_t desc;
    memcpy(&desc, s_head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, why_len, "no application description in the image");
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    desc.project_name[sizeof(desc.project_name) - 1] = '\0';
    desc.version[sizeof(desc.version) - 1] = '\0';
    const esp_app_desc_t *self = esp_app_get_description();
    if (!(s_flags & OTA_CORE_ANY_PROJECT) && strcmp(desc.project_name, self->project_name) != 0) {
        snprintf(why, why_len, "built for another project (%s, this is %s)", desc.project_name,
                 self->project_name);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    lock();
    s_session.app = desc;
    s_session.have_app = true;
    unlock();
    return ESP_OK;
}

esp_err_t ota_core_write(const void *data, size_t len)
{
    char why[128] = "";
    esp_err_t err = ESP_OK;
    const size_t before = s_session.bytes;      /* only the owner changes it */

    if (s_cancel) {
        snprintf(why, sizeof(why), "cancelled");
        err = ESP_ERR_INVALID_STATE;
    } else if (s_session.total && before + len > s_session.total) {
        snprintf(why, sizeof(why), "more than the %u bytes announced", (unsigned)s_session.total);
        err = ESP_ERR_INVALID_SIZE;
    } else if (before + len > s_part->size) {
        snprintf(why, sizeof(why), "larger than the %" PRIu32 " KB slot", s_part->size / 1024);
        err = ESP_ERR_INVALID_SIZE;
    }

    if (err == ESP_OK && !s_head_checked) {
        const size_t take = before < HEAD_LEN ? (HEAD_LEN - before < len ? HEAD_LEN - before : len) : 0;
        memcpy(s_head + before, data, take);
        if (before + take >= HEAD_LEN) {
            s_head_checked = true;
            err = check_head(why, sizeof(why));
        }
    }
    if (err == ESP_OK && s_open) {
        err = esp_ota_write(s_handle, data, len);
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "%s", err == ESP_ERR_OTA_VALIDATE_FAILED
                     ? "not an application image" : esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        psa_hash_update(&s_sha, data, len);
        lock();
        s_session.bytes = before + len;
        unlock();
        return ESP_OK;
    }

    release();
    lock();
    set_failed_locked(why);
    unlock();
    ESP_LOGW(TAG, "update failed after %u bytes: %s", (unsigned)before, why);
    return err;
}

static void hex_of(const uint8_t *digest, size_t n, char *hex)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
}

esp_err_t ota_core_finish(const char *expect_sha256)
{
    char why[128] = "";
    esp_err_t err = ESP_OK;

    lock();
    s_session.state = OTA_CORE_VERIFYING;
    const size_t bytes = s_session.bytes;
    const size_t total = s_session.total;
    unlock();

    uint8_t digest[32];
    size_t n = 0;
    char hex[65] = "";
    s_hashing = false;
    if (psa_hash_finish(&s_sha, digest, sizeof(digest), &n) == PSA_SUCCESS && n == sizeof(digest)) {
        hex_of(digest, n, hex);
    } else {
        psa_hash_abort(&s_sha);
    }
    lock();
    strlcpy(s_session.sha256, hex, sizeof(s_session.sha256));
    unlock();

    if (s_cancel) {
        snprintf(why, sizeof(why), "cancelled");
        err = ESP_ERR_INVALID_STATE;
    } else if (total && bytes != total) {
        snprintf(why, sizeof(why), "short: %u of %u bytes", (unsigned)bytes, (unsigned)total);
        err = ESP_ERR_INVALID_SIZE;
    } else if (bytes < HEAD_LEN) {
        snprintf(why, sizeof(why), "only %u bytes: not an application image", (unsigned)bytes);
        err = ESP_ERR_INVALID_SIZE;
    } else if (expect_sha256 != NULL && expect_sha256[0] != '\0' && strcasecmp(expect_sha256, hex) != 0) {
        snprintf(why, sizeof(why), "SHA-256 mismatch: received %.12s..., expected %.12s...", hex, expect_sha256);
        err = ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_OK && s_session.dry_run) {
        lock();
        s_session.state = OTA_CORE_IDLE;
        s_session.finished_us = esp_timer_get_time();
        unlock();
        return ESP_OK;
    }
    if (err == ESP_OK) {
        s_open = false;
        err = esp_ota_end(s_handle);        /* frees the handle whatever it returns */
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "the image did not verify (%s)", esp_err_to_name(err));
        }
    }
    if (err != ESP_OK) {
        release();
        lock();
        set_failed_locked(why);
        unlock();
        ESP_LOGW(TAG, "update failed: %s", why);
        return err;
    }

    esp_app_desc_t desc;
    const bool have = esp_ota_get_partition_description(s_part, &desc) == ESP_OK;
    lock();
    s_session.state = OTA_CORE_STAGED;
    if (have) {
        s_session.app = desc;
        s_session.have_app = true;
    }
    s_session.finished_us = esp_timer_get_time();
    unlock();
    ESP_LOGI(TAG, "%s staged in %s", have ? desc.version : "image", s_part->label);
    return ESP_OK;
}

void ota_core_fail(const char *fmt, ...)
{
    char why[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    release();
    lock();
    if (in_progress(s_session.state)) {
        /* A cancel is the reason, whatever the owner saw as a result of it */
        set_failed_locked(s_cancel ? "cancelled" : why);
    }
    unlock();
    ESP_LOGW(TAG, "update failed: %s", why);
}

esp_err_t ota_core_discard(void)
{
    lock();
    esp_err_t err = ESP_OK;
    switch (s_session.state) {
    case OTA_CORE_RECEIVING:
        s_cancel = true;
        break;
    case OTA_CORE_VERIFYING:
    case OTA_CORE_ACTIVE:
        err = ESP_ERR_INVALID_STATE;
        break;
    default:
        /* A staged image stays in its slot, harmless: nothing boots it, and the next update
         * overwrites it. */
        memset(&s_session, 0, sizeof(s_session));
        break;
    }
    unlock();
    return err;
}

esp_err_t ota_core_activate(const char *slot)
{
    const esp_partition_t *part = NULL;
    lock();
    const ota_core_state_t st = s_session.state;
    unlock();

    if (slot == NULL || slot[0] == '\0') {
        if (st != OTA_CORE_STAGED || s_part == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        part = s_part;
    } else {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, slot);
        if (part == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        if (in_progress(st) && s_part == part) {
            return ESP_ERR_INVALID_STATE;       /* being written */
        }
        esp_ota_img_states_t img;
        if (esp_ota_get_state_partition(part, &img) == ESP_OK &&
            (img == ESP_OTA_IMG_INVALID || img == ESP_OTA_IMG_ABORTED)) {
            return ESP_ERR_OTA_VALIDATE_FAILED;
        }
    }
    /* Verifies the image in the slot before selecting it */
    const esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return err;
    }
    esp_app_desc_t desc;
    const bool have = esp_ota_get_partition_description(part, &desc) == ESP_OK;
    lock();
    if (st != OTA_CORE_STAGED || part != s_part) {
        /* Switching to a slot no session wrote: the session now describes that */
        memset(&s_session, 0, sizeof(s_session));
        s_session.have_app = have;
        if (have) {
            s_session.app = desc;
        }
    }
    s_session.state = OTA_CORE_ACTIVE;
    strlcpy(s_session.slot, part->label, sizeof(s_session.slot));
    unlock();
    ESP_LOGI(TAG, "%s (%s) now boots", part->label, have ? desc.version : "?");
    return ESP_OK;
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void ota_core_restart_after(uint32_t delay_ms)
{
    if (s_restart_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = restart_cb, .name = "ota_restart" };
        if (esp_timer_create(&args, &s_restart_timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, (uint64_t)(delay_ms ? delay_ms : 1) * 1000);
}
