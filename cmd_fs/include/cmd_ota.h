/*
 * `ota`: application updates over the console with XMODEM-1K -- into the OTA slot that is not
 * running, verified, and made the boot image. Host side: tools/fs_xfer.py ota.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register `ota`. `uart_num` is the UART transfers run on; -1 for the console's. */
esp_err_t register_ota(int uart_num);

/*
 * The running image is good. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an image an update
 * installed boots on trial, and a reset before this is called goes back to the image before
 * it. Call it once the application is up -- its console running, say. Does nothing for an
 * image already confirmed or flashed over USB, or without rollback.
 */
void ota_confirm_running(void);

#ifdef __cplusplus
}
#endif
