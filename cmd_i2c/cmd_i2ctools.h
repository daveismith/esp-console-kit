/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the i2cconfig / i2cdetect / i2cget / i2cset / i2cdump commands.
 *
 * The commands attach to whatever I2C master bus the application has already
 * created on the default port (see I2C_TOOL_DEFAULT_PORT in cmd_i2ctools.c),
 * resolved lazily on first use. Nothing has to be wired up at registration
 * time, and the bus this borrows is not torn down by `i2cconfig`.
 */
void register_i2ctools(void);

#ifdef __cplusplus
}
#endif
