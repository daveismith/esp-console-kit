/*
 * web_ota -- application updates over HTTP: the /api/v1/ota routes, over ota_core and
 * ota_pull. See web_ota.c.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the routes, list "ota" as a feature, and make ota_pull the console's puller. */
esp_err_t web_ota_register(void);

/* -1, 0 or 1 for two release tags (vX.Y.Z or vX.Y.Z-rcN); 2 when either is not one. */
int web_ota_compare_versions(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
