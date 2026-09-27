/*
 * web_fs -- the storage volume over HTTP: the /api/v1/fs routes, over fs_ops. See web_fs.c,
 * and the OpenAPI description the application serves.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "fs_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the routes and list "files" as a feature. After register_fs() (or fs_ops_init()). */
esp_err_t web_fs_register(void);

/* For other routes that take a path (a clip to show): `rel`, from the volume's root, as an
 * absolute path -- false after sending 400 bad_path. */
bool web_fs_resolve(httpd_req_t *req, const char *rel, char *abs, size_t abs_len);

/* Send an fs_ops errno as the API's error for `abs`: 404 not_found, 409 exists, ... */
esp_err_t web_fs_send_errno(httpd_req_t *req, int err, const char *abs);

/* An entry as the API describes it: name, path (from the volume's root), type, size, mtime. */
cJSON *web_fs_entry_json(const char *abs, const fs_entry_t *e);

#ifdef __cplusplus
}
#endif
