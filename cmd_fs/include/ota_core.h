/*
 * ota_core -- one application update at a time, whoever brings the image: `ota put` over
 * XMODEM, an HTTP upload, or a pull from a URL. See ota_core.c.
 *
 * An update is a session. Its owner calls begin, write as the bytes arrive, then finish,
 * which has ESP-IDF verify the image (header, chip, SHA-256) and leaves it *staged*: written
 * and good, but not yet the boot image. activate makes it the boot image. Anyone may read the
 * session at any time, and anyone may cancel it; the owner's next write then fails.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CORE_IDLE,        /* nothing since boot, or a staged image discarded */
    OTA_CORE_RECEIVING,   /* bytes arriving (an upload, a transfer, a download) */
    OTA_CORE_VERIFYING,   /* all there; ESP-IDF is checking the image */
    OTA_CORE_STAGED,      /* verified, in its slot, not the boot image */
    OTA_CORE_ACTIVE,      /* the boot image: it runs after a restart */
    OTA_CORE_FAILED,      /* see `error`; the running image still boots */
} ota_core_state_t;

typedef enum {
    OTA_CORE_SRC_NONE,
    OTA_CORE_SRC_CONSOLE,
    OTA_CORE_SRC_UPLOAD,
    OTA_CORE_SRC_PULL,
} ota_core_source_t;

/* esp_ota begin flags */
#define OTA_CORE_ERASE_FIRST    (1u << 0)   /* erase the whole size up front, not as written */
#define OTA_CORE_DRY_RUN        (1u << 1)   /* receive and hash, write nothing */
#define OTA_CORE_ANY_PROJECT    (1u << 2)   /* accept an image built for another project */

typedef struct {
    ota_core_state_t state;
    ota_core_source_t source;
    bool dry_run;
    char slot[17];              /* the partition being written, or the one made to boot */
    char url[256];              /* for a pull: what was fetched */
    size_t bytes;               /* received so far */
    size_t total;               /* expected; 0 when not known */
    char sha256[65];            /* of the image as received, once finished */
    bool have_app;              /* `app` holds the image's descriptor */
    esp_app_desc_t app;
    char error[128];
    int64_t started_us;         /* esp_timer time */
    int64_t finished_us;        /* 0 while running */
} ota_core_session_t;

typedef struct {
    char label[17];
    uint32_t address;
    uint32_t size;
    bool running;
    bool boots;                 /* the boot image: runs after a restart */
    bool next;                  /* the slot an update writes */
    bool have_app;
    esp_app_desc_t app;
    bool have_state;            /* an OTA record exists for this slot */
    int state;                  /* esp_ota_img_states_t when have_state */
} ota_core_slot_t;

/* Called as a session begins writing (not for a dry run), from the owner's task. The
 * application uses it to stop what flash writes would disturb, a video, say. */
typedef void (*ota_core_hook_t)(void *ctx);
void ota_core_set_begin_hook(ota_core_hook_t hook, void *ctx);

/*
 * Start a session into the slot that isn't running. `size` is the image's size, or 0 when it
 * is not known yet (a chunked download); `url` is recorded for a pull, else NULL.
 *   ESP_ERR_INVALID_STATE  another session is receiving or verifying
 *   ESP_ERR_INVALID_SIZE   it will not fit the slot
 *   ESP_ERR_NOT_FOUND      no second app slot in the partition table
 * On any error there is no session to finish or abort.
 */
esp_err_t ota_core_begin(ota_core_source_t source, size_t size, const char *url, unsigned flags);

/* The owner learns what it is fetching after it began (a pull resolving a release): the URL
 * and, if known (else 0), the size. Only before the first write. */
void ota_core_set_source(const char *url, size_t total);

/* Append bytes. Fails -- and the session is failed -- when the image is not an application,
 * is for another chip or project, overruns its size, or the session was cancelled. The first
 * such check runs on the image's first 288 bytes, so a wrong file fails at once. */
esp_err_t ota_core_write(const void *data, size_t len);

/*
 * All the bytes are in: check the size, the SHA-256 against `expect_sha256` (hex, or NULL),
 * and have ESP-IDF verify the image. Staged on success (ESP_OK); failed otherwise, with the
 * reason in the session. A dry run ends idle with the hash filled in.
 */
esp_err_t ota_core_finish(const char *expect_sha256);

/* The owner gives up, with a reason (a dropped connection, a download error). */
void ota_core_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Anyone: cancel a session in progress (its owner's next write fails), or forget a staged
 * image. ESP_ERR_INVALID_STATE when the session is already the boot image. */
esp_err_t ota_core_discard(void);

/*
 * Make a slot the boot image. NULL means the staged image. A named slot must hold a verified
 * application that was not rolled back or marked invalid. ESP_ERR_INVALID_STATE with nothing
 * staged; ESP_ERR_NOT_FOUND for an unknown slot; ESP_ERR_OTA_VALIDATE_FAILED for a slot
 * without a good image. On success the session is ACTIVE.
 */
esp_err_t ota_core_activate(const char *slot);

/* esp_restart() after `delay_ms`, from a timer, so a caller can finish replying first. */
void ota_core_restart_after(uint32_t delay_ms);

void ota_core_get(ota_core_session_t *out);
bool ota_core_busy(void);
const char *ota_core_state_name(ota_core_state_t state);
const char *ota_core_source_name(ota_core_source_t source);

/* Every application slot. Returns how many were written to `out` (at most `max`). */
size_t ota_core_slots(ota_core_slot_t *out, size_t max);
const char *ota_core_img_state_name(int state);     /* esp_ota_img_states_t -> words */
const char *ota_core_img_state_key(int state);      /* esp_ota_img_states_t -> an API word */

/*
 * A pull: fetch an image from `what` (a URL, or a name the puller understands, such as a
 * release channel) and stage it. Whoever can pull (an HTTP client component) installs the
 * handler, so `ota pull` works without the console depending on HTTP. The handler returns
 * once the pull has started; the session reports its progress.
 */
typedef struct {
    const char *what;
    const char *sha256;         /* expected, hex, or NULL */
    bool activate;              /* make it the boot image once staged */
    bool reboot;                /* and restart into it */
    bool any_project;
} ota_core_pull_req_t;

typedef esp_err_t (*ota_core_pull_fn)(const ota_core_pull_req_t *req, char *why, size_t why_len);
void ota_core_set_puller(ota_core_pull_fn fn);
ota_core_pull_fn ota_core_puller(void);

#ifdef __cplusplus
}
#endif
