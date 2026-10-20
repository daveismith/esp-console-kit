/* Console example — declarations of command registration functions.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#include "nvs_backup.h"   /* nvs_backup_config_t, nvs_backup_exclude() */

#ifdef __cplusplus
extern "C" {
#endif

// Register NVS functions
void register_nvs(void);

// The configuration backup: nvs_backup_init(cfg), then `nvs_inventory` and `nvs_export`
void register_nvs_backup(const nvs_backup_config_t *cfg);

#ifdef __cplusplus
}
#endif
