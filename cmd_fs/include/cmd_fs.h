/*
 * `fs`: manage a mounted filesystem from the console, and move files over the console UART
 * with XMODEM. Host side: tools/fs_xfer.py.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Mount point of the volume, e.g. "/data". Paths given to `fs` are relative to it:
     * "clip.mov", "/clip.mov" and "/data/clip.mov" all name the same file. Copied. */
    const char *base_path;
    /* Optional, for `fs df` and the free-space check before an upload: total and used bytes
     * (esp_littlefs_info(), esp_vfs_fat_info(), ...). */
    esp_err_t (*info)(void *ctx, size_t *total_bytes, size_t *used_bytes);
    void *info_ctx;
    /* The UART `fs put` / `fs get` talk XMODEM on; -1 for the console UART. */
    int uart_num;
} cmd_fs_config_t;

esp_err_t register_fs(const cmd_fs_config_t *config);

#ifdef __cplusplus
}
#endif
