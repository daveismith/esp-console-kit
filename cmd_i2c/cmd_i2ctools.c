/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* cmd_i2ctools.c

   Derived from the ESP-IDF i2c-tools example, adapted for IDF 6.x and for a
   board that brings its own I2C bus up before the console exists.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "cmd_i2ctools";

#define I2C_TOOL_TIMEOUT_VALUE_MS (50)

/* The port the application is expected to have put its board bus on. A session
 * can attach elsewhere with `i2cconfig --port`. */
#define I2C_TOOL_DEFAULT_PORT     (0)

/* Largest -l/-d payload a single command will handle. */
#define I2C_TOOL_MAX_DATA_LEN     (256)

/* 100 kHz, not 400: standard-mode-only parts (a DS1307, say) will not answer
 * above 100 kHz. The clock is a property of the device handle, not of the bus,
 * so slow devices and fast ones coexist on one bus -- but the default has to
 * suit the slowest thing likely to be plugged in. `i2cconfig --freq` changes it. */
static uint32_t i2c_frequency = 100 * 1000;

/* The bus the commands operate on. Resolved on first use rather than created at
 * registration time: the application's board bring-up has usually created a
 * master bus on I2C_TOOL_DEFAULT_PORT by the time the console starts, and a
 * second i2c_new_master_bus() on an occupied port fails. */
static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_port_num_t s_i2c_port = I2C_TOOL_DEFAULT_PORT;
static bool s_bus_owned = false;    /* true only if i2cconfig created s_bus_handle */
static int s_gpio_sda = -1;         /* only meaningful while s_bus_owned */
static int s_gpio_scl = -1;

static esp_err_t i2c_check_port(int port, i2c_port_num_t *i2c_port)
{
    if (port < 0 || port >= SOC_HP_I2C_NUM) {
        ESP_LOGE(TAG, "Wrong port number: %d (expected 0..%d)", port, SOC_HP_I2C_NUM - 1);
        return ESP_ERR_INVALID_ARG;
    }
    *i2c_port = port;
    return ESP_OK;
}

static bool i2c_check_addr(int chip_addr)
{
    if (chip_addr < 0 || chip_addr > 0x7f) {
        ESP_LOGE(TAG, "Wrong chip address: 0x%x (expected 0x00..0x7f)", chip_addr);
        return false;
    }
    return true;
}

/**
 * @brief Hand back the bus to work on, borrowing the application's if it has one.
 */
static esp_err_t i2c_tool_get_bus(i2c_master_bus_handle_t *out_handle)
{
    if (s_bus_handle == NULL) {
        esp_err_t err = i2c_master_get_bus_handle(s_i2c_port, &s_bus_handle);
        if (err != ESP_OK) {
            s_bus_handle = NULL;
            ESP_LOGE(TAG, "No I2C master bus on port %d. Create one with "
                     "'i2cconfig --port %d --sda <gpio> --scl <gpio>'.", s_i2c_port, s_i2c_port);
            return err;
        }
        s_bus_owned = false;
    }
    *out_handle = s_bus_handle;
    return ESP_OK;
}

/**
 * @brief Add a temporary device handle for one command's worth of transfers.
 */
static esp_err_t i2c_tool_open_device(int chip_addr, i2c_master_dev_handle_t *out_dev)
{
    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_tool_get_bus(&bus_handle);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t i2c_dev_conf = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = chip_addr,
        .scl_speed_hz = i2c_frequency,
    };
    err = i2c_master_bus_add_device(bus_handle, &i2c_dev_conf, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device 0x%02x: %s", chip_addr, esp_err_to_name(err));
    }
    return err;
}

static void i2c_tool_print_config(void)
{
    printf("I2C port %d, %" PRIu32 " Hz", s_i2c_port, i2c_frequency);
    if (s_bus_owned) {
        printf(", bus created by i2cconfig on sda=%d scl=%d\n", s_gpio_sda, s_gpio_scl);
    } else if (s_bus_handle != NULL) {
        printf(", using the bus the application already created\n");
    } else {
        printf(", bus not resolved yet\n");
    }
}

static struct {
    struct arg_int *port;
    struct arg_int *freq;
    struct arg_int *sda;
    struct arg_int *scl;
    struct arg_end *end;
} i2cconfig_args;

static int do_i2cconfig_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cconfig_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cconfig_args.end, argv[0]);
        return 1;
    }

    i2c_port_num_t i2c_port = s_i2c_port;
    if (i2cconfig_args.port->count) {
        if (i2c_check_port(i2cconfig_args.port->ival[0], &i2c_port) != ESP_OK) {
            return 1;
        }
    }

    if (i2cconfig_args.freq->count) {
        int freq = i2cconfig_args.freq->ival[0];
        if (freq <= 0) {
            ESP_LOGE(TAG, "Wrong frequency: %d", freq);
            return 1;
        }
        i2c_frequency = (uint32_t)freq;
    }

    /* --sda/--scl are what turn this into a bus (re)creation; without them the
     * command only retunes the clock or selects a different port. Upstream made
     * them mandatory, which meant every frequency change tore the bus down. */
    if (i2cconfig_args.sda->count != i2cconfig_args.scl->count) {
        ESP_LOGE(TAG, "--sda and --scl must be given together");
        return 1;
    }

    if (i2cconfig_args.sda->count == 0) {
        if (i2c_port != s_i2c_port) {
            s_i2c_port = i2c_port;
            s_bus_handle = NULL;    /* re-resolve against the new port on next use */
            s_bus_owned = false;
        }
        i2c_tool_print_config();
        return 0;
    }

    /* Refuse to delete a bus we did not create: the application's drivers hold
     * device handles on it, and tearing it down pulls it out from under them. */
    i2c_master_bus_handle_t existing = NULL;
    bool port_busy = (i2c_master_get_bus_handle(i2c_port, &existing) == ESP_OK);
    if (port_busy && !(s_bus_owned && existing == s_bus_handle)) {
        ESP_LOGE(TAG, "I2C port %d already has a bus owned by the application; "
                 "refusing to tear it down. "
                 "Use 'i2cconfig --port %d' to attach to it as-is.", i2c_port, i2c_port);
        return 1;
    }

    if (s_bus_owned && s_bus_handle != NULL) {
        /* Safe: every command removes its device handle before returning, so
         * nothing is attached to the bus between commands. */
        esp_err_t err = i2c_del_master_bus(s_bus_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete the previous bus: %s", esp_err_to_name(err));
            return 1;
        }
        s_bus_handle = NULL;
        s_bus_owned = false;
    }

    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = i2c_port,
        .scl_io_num = i2cconfig_args.scl->ival[0],
        .sda_io_num = i2cconfig_args.sda->ival[0],
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&i2c_bus_config, &s_bus_handle);
    if (err != ESP_OK) {
        s_bus_handle = NULL;
        ESP_LOGE(TAG, "Failed to create I2C bus on port %d: %s", i2c_port, esp_err_to_name(err));
        return 1;
    }
    s_i2c_port = i2c_port;
    s_bus_owned = true;
    s_gpio_sda = i2cconfig_args.sda->ival[0];
    s_gpio_scl = i2cconfig_args.scl->ival[0];

    i2c_tool_print_config();
    return 0;
}

static void register_i2cconfig(void)
{
    i2cconfig_args.port = arg_int0(NULL, "port", "<0|1>", "Set the I2C bus port number");
    i2cconfig_args.freq = arg_int0(NULL, "freq", "<Hz>", "Set the frequency(Hz) of I2C bus");
    i2cconfig_args.sda = arg_int0(NULL, "sda", "<gpio>", "Set the gpio for I2C SDA (creates a bus; needs --scl)");
    i2cconfig_args.scl = arg_int0(NULL, "scl", "<gpio>", "Set the gpio for I2C SCL (creates a bus; needs --sda)");
    i2cconfig_args.end = arg_end(2);
    const esp_console_cmd_t i2cconfig_cmd = {
        .command = "i2cconfig",
        .help = "Config I2C bus (no options: show the current configuration)",
        .hint = NULL,
        .func = &do_i2cconfig_cmd,
        .argtable = &i2cconfig_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cconfig_cmd));
}

static int do_i2cdetect_cmd(int argc, char **argv)
{
    i2c_master_bus_handle_t bus_handle;
    if (i2c_tool_get_bus(&bus_handle) != ESP_OK) {
        return 1;
    }

    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            uint8_t address = i + j;
            esp_err_t ret = i2c_master_probe(bus_handle, address, I2C_TOOL_TIMEOUT_VALUE_MS);
            if (ret == ESP_OK) {
                printf("%02x ", address);
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }

    return 0;
}

static void register_i2cdetect(void)
{
    const esp_console_cmd_t i2cdetect_cmd = {
        .command = "i2cdetect",
        .help = "Scan I2C bus for devices",
        .hint = NULL,
        .func = &do_i2cdetect_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cdetect_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *register_address;
    struct arg_int *data_length;
    struct arg_end *end;
} i2cget_args;

static int do_i2cget_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cget_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cget_args.end, argv[0]);
        return 1;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cget_args.chip_address->ival[0];
    if (!i2c_check_addr(chip_addr)) {
        return 1;
    }
    /* Check register address: "-r" option. Negative means "no register": read
     * straight from the device rather than writing a pointer byte first.
     * Upstream left this as -1 and then handed &data_addr to the transmit, so
     * an omitted -r silently wrote 0xff as the register address. */
    int data_addr = -1;
    if (i2cget_args.register_address->count) {
        data_addr = i2cget_args.register_address->ival[0];
        if (data_addr < 0 || data_addr > 0xff) {
            ESP_LOGE(TAG, "Wrong register address: 0x%x (expected 0x00..0xff)", data_addr);
            return 1;
        }
    }
    /* Check data length: "-l" option */
    int len = 1;
    if (i2cget_args.data_length->count) {
        len = i2cget_args.data_length->ival[0];
    }
    if (len <= 0 || len > I2C_TOOL_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Wrong read length: %d (expected 1..%d)", len, I2C_TOOL_MAX_DATA_LEN);
        return 1;
    }

    i2c_master_dev_handle_t dev_handle;
    if (i2c_tool_open_device(chip_addr, &dev_handle) != ESP_OK) {
        return 1;
    }

    uint8_t *data = malloc(len);
    if (data == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        i2c_master_bus_rm_device(dev_handle);
        return 1;
    }

    esp_err_t ret;
    if (data_addr >= 0) {
        uint8_t reg = (uint8_t)data_addr;
        ret = i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, I2C_TOOL_TIMEOUT_VALUE_MS);
    } else {
        ret = i2c_master_receive(dev_handle, data, len, I2C_TOOL_TIMEOUT_VALUE_MS);
    }

    if (ret == ESP_OK) {
        for (int i = 0; i < len; i++) {
            printf("0x%02x ", data[i]);
            if ((i + 1) % 16 == 0) {
                printf("\r\n");
            }
        }
        if (len % 16) {
            printf("\r\n");
        }
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Bus is busy");
    } else {
        ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
    }
    free(data);
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return ret == ESP_OK ? 0 : 1;
}

static void register_i2cget(void)
{
    i2cget_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cget_args.register_address = arg_int0("r", "register", "<register_addr>", "Specify the address on that chip to read from");
    i2cget_args.data_length = arg_int0("l", "length", "<length>", "Specify the length to read from that data address");
    i2cget_args.end = arg_end(1);
    const esp_console_cmd_t i2cget_cmd = {
        .command = "i2cget",
        .help = "Read registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cget_cmd,
        .argtable = &i2cget_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cget_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *register_address;
    struct arg_int *data;
    struct arg_end *end;
} i2cset_args;

static int do_i2cset_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cset_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cset_args.end, argv[0]);
        return 1;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cset_args.chip_address->ival[0];
    if (!i2c_check_addr(chip_addr)) {
        return 1;
    }
    /* Check register address: "-r" option */
    int data_addr = 0;
    if (i2cset_args.register_address->count) {
        data_addr = i2cset_args.register_address->ival[0];
        if (data_addr < 0 || data_addr > 0xff) {
            ESP_LOGE(TAG, "Wrong register address: 0x%x (expected 0x00..0xff)", data_addr);
            return 1;
        }
    }
    /* Check data: trailing positional arguments */
    int len = i2cset_args.data->count;
    for (int i = 0; i < len; i++) {
        int value = i2cset_args.data->ival[i];
        if (value < 0 || value > 0xff) {
            ESP_LOGE(TAG, "Wrong data byte: 0x%x (expected 0x00..0xff)", value);
            return 1;
        }
    }

    i2c_master_dev_handle_t dev_handle;
    if (i2c_tool_open_device(chip_addr, &dev_handle) != ESP_OK) {
        return 1;
    }

    uint8_t *data = malloc(len + 1);
    if (data == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        i2c_master_bus_rm_device(dev_handle);
        return 1;
    }
    data[0] = (uint8_t)data_addr;
    for (int i = 0; i < len; i++) {
        data[i + 1] = (uint8_t)i2cset_args.data->ival[i];
    }
    esp_err_t ret = i2c_master_transmit(dev_handle, data, len + 1, I2C_TOOL_TIMEOUT_VALUE_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Write OK");
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Bus is busy");
    } else {
        ESP_LOGW(TAG, "Write failed: %s", esp_err_to_name(ret));
    }

    free(data);
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return ret == ESP_OK ? 0 : 1;
}

static void register_i2cset(void)
{
    i2cset_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cset_args.register_address = arg_int0("r", "register", "<register_addr>", "Specify the address on that chip to write to");
    i2cset_args.data = arg_intn(NULL, NULL, "<data>", 0, I2C_TOOL_MAX_DATA_LEN, "Specify the data to write to that data address");
    i2cset_args.end = arg_end(2);
    const esp_console_cmd_t i2cset_cmd = {
        .command = "i2cset",
        .help = "Set registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cset_cmd,
        .argtable = &i2cset_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cset_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *size;
    struct arg_end *end;
} i2cdump_args;

static int do_i2cdump_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cdump_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cdump_args.end, argv[0]);
        return 1;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cdump_args.chip_address->ival[0];
    if (!i2c_check_addr(chip_addr)) {
        return 1;
    }
    /* Check read size: "-s" option */
    int size = 1;
    if (i2cdump_args.size->count) {
        size = i2cdump_args.size->ival[0];
    }
    if (size != 1 && size != 2 && size != 4) {
        ESP_LOGE(TAG, "Wrong read size. Only support 1,2,4");
        return 1;
    }

    i2c_master_dev_handle_t dev_handle;
    if (i2c_tool_open_device(chip_addr, &dev_handle) != ESP_OK) {
        return 1;
    }

    uint8_t data_addr;
    uint8_t data[4];
    int32_t block[16];
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f"
           "    0123456789abcdef\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j += size) {
            fflush(stdout);
            data_addr = i + j;
            esp_err_t ret = i2c_master_transmit_receive(dev_handle, &data_addr, 1, data, size, I2C_TOOL_TIMEOUT_VALUE_MS);
            if (ret == ESP_OK) {
                for (int k = 0; k < size; k++) {
                    printf("%02x ", data[k]);
                    block[j + k] = data[k];
                }
            } else {
                for (int k = 0; k < size; k++) {
                    printf("XX ");
                    block[j + k] = -1;
                }
            }
        }
        printf("   ");
        for (int k = 0; k < 16; k++) {
            /* Upstream fell through here: a failed read printed "X" and then a
             * second character, because -1 & 0xff is 0xff. */
            if (block[k] < 0) {
                printf("X");
            } else if ((block[k] & 0xff) == 0x00 || (block[k] & 0xff) == 0xff) {
                printf(".");
            } else if ((block[k] & 0xff) < 32 || (block[k] & 0xff) >= 127) {
                printf("?");
            } else {
                printf("%c", (char)(block[k] & 0xff));
            }
        }
        printf("\r\n");
    }
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return 0;
}

static void register_i2cdump(void)
{
    i2cdump_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cdump_args.size = arg_int0("s", "size", "<size>", "Specify the size of each read");
    i2cdump_args.end = arg_end(1);
    const esp_console_cmd_t i2cdump_cmd = {
        .command = "i2cdump",
        .help = "Examine registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cdump_cmd,
        .argtable = &i2cdump_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cdump_cmd));
}

void register_i2ctools(void)
{
    register_i2cconfig();
    register_i2cdetect();
    register_i2cget();
    register_i2cset();
    register_i2cdump();
}
