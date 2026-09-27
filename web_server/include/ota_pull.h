/*
 * ota_pull -- the board fetches its own update: from an image URL, a release manifest, or a
 * release channel an application knows how to resolve. The bytes go through ota_core, so a
 * pull is a session like any other. See ota_pull.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ota_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a pull resolves to. */
typedef struct {
    char channel[24];           /* the channel asked for, if any */
    char version[32];           /* from a manifest; empty for a bare image URL */
    char project[32];
    char image_url[256];
    char release_url[160];      /* a page about the release, if the manifest names one */
    char sha256[65];            /* expected, from the manifest; empty if unknown */
    size_t size;                /* from the manifest; 0 if unknown */
} ota_pull_target_t;

/*
 * A release channel's manifest URL. The application installs one (the release pipeline is
 * its business); `channel` is anything without "://", such as "latest".
 */
typedef esp_err_t (*ota_pull_resolver_t)(const char *channel, char *manifest_url, size_t url_len,
                                         char *why, size_t why_len);
void ota_pull_set_resolver(ota_pull_resolver_t resolver);
bool ota_pull_has_channels(void);

/*
 * Resolve `what` -- a channel, a manifest URL (ending .json), or an image URL -- to what would be
 * downloaded, without downloading it. A manifest is JSON with `project`, `version`, and a
 * `parts` array whose `role: "app"` entry gives `path` (relative to the manifest), `size` and
 * `sha256` -- the format tools/web_install_manifest.py writes. Blocks for the fetches.
 */
esp_err_t ota_pull_resolve(const char *what, ota_pull_target_t *out, char *why, size_t why_len);

/* GET `url` into a malloc'd, NUL-terminated buffer of at most `max` bytes. Follows redirects,
 * but never from https to http. For resolvers. */
esp_err_t ota_pull_fetch(const char *url, char **body, size_t max, char *why, size_t why_len);

/* Start a pull in the background (the ota_core puller). ESP_ERR_INVALID_STATE when an update is
 * already running. Installs itself with ota_core_set_puller() at the first web_ota_register(). */
esp_err_t ota_pull_start(const ota_core_pull_req_t *req, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif
