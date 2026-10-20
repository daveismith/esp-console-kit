/* Console example — NVS commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "cmd_nvs.h"
#include "nvs.h"
#include "nvs_backup.h"
#include "fs_ops.h"

/* The names are the backup format's (nvs_backup_type_name), so a .r2cfg file reads the same. */
static const char *ARG_TYPE_STR = "type can be: i8, u8, i16, u16 i32, u32 i64, u64, str, blob";
static char current_namespace[16] = "storage";
static const char *TAG = "cmd_nvs";

static struct {
    struct arg_str *key;
    struct arg_str *type;
    struct arg_str *value;
    struct arg_end *end;
} set_args;

static struct {
    struct arg_str *key;
    struct arg_str *type;
    struct arg_end *end;
} get_args;

static struct {
    struct arg_str *key;
    struct arg_end *end;
} erase_args;

static struct {
    struct arg_str *namespace;
    struct arg_end *end;
} erase_all_args;

static struct {
    struct arg_str *namespace;
    struct arg_end *end;
} namespace_args;

static struct {
    struct arg_str *partition;
    struct arg_str *namespace;
    struct arg_str *type;
    struct arg_end *end;
} list_args;


static esp_err_t store_blob(nvs_handle_t nvs, const char *key, const char *str_values)
{
    uint8_t value;
    size_t str_len = strlen(str_values);
    size_t blob_len = str_len / 2;

    if (str_len % 2) {
        ESP_LOGE(TAG, "Blob data must contain even number of characters");
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    char *blob = (char *)malloc(blob_len);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0, j = 0; i < str_len; i++) {
        char ch = str_values[i];
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'A' && ch <= 'F') {
            value = ch - 'A' + 10;
        } else if (ch >= 'a' && ch <= 'f') {
            value = ch - 'a' + 10;
        } else {
            ESP_LOGE(TAG, "Blob data contain invalid character");
            free(blob);
            return ESP_ERR_NVS_TYPE_MISMATCH;
        }

        if (i & 1) {
            blob[j++] += value;
        } else {
            blob[j] = value << 4;
        }
    }

    esp_err_t err = nvs_set_blob(nvs, key, blob, blob_len);
    free(blob);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    return err;
}

static void print_blob(const char *blob, size_t len)
{
    for (int i = 0; i < len; i++) {
        printf("%02x", blob[i]);
    }
    printf("\n");
}


static esp_err_t set_value_in_nvs(const char *key, const char *str_type, const char *str_value)
{
    esp_err_t err;
    nvs_handle_t nvs;
    bool range_error = false;

    nvs_type_t type = nvs_backup_type_parse(str_type);

    if (type == NVS_TYPE_ANY) {
        ESP_LOGE(TAG, "Type '%s' is undefined", str_type);
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    err = nvs_open(current_namespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (type == NVS_TYPE_I8) {
        int32_t value = strtol(str_value, NULL, 0);
        if (value < INT8_MIN || value > INT8_MAX || errno == ERANGE) {
            range_error = true;
        } else {
            err = nvs_set_i8(nvs, key, (int8_t)value);
        }
    } else if (type == NVS_TYPE_U8) {
        uint32_t value = strtoul(str_value, NULL, 0);
        if (value > UINT8_MAX || errno == ERANGE) {
            range_error = true;
        } else {
            err = nvs_set_u8(nvs, key, (uint8_t)value);
        }
    } else if (type == NVS_TYPE_I16) {
        int32_t value = strtol(str_value, NULL, 0);
        if (value < INT16_MIN || value > INT16_MAX || errno == ERANGE) {
            range_error = true;
        } else {
            err = nvs_set_i16(nvs, key, (int16_t)value);
        }
    } else if (type == NVS_TYPE_U16) {
        uint32_t value = strtoul(str_value, NULL, 0);
        if (value > UINT16_MAX || errno == ERANGE) {
            range_error = true;
        } else {
            err = nvs_set_u16(nvs, key, (uint16_t)value);
        }
    } else if (type == NVS_TYPE_I32) {
        int32_t value = strtol(str_value, NULL, 0);
        if (errno != ERANGE) {
            err = nvs_set_i32(nvs, key, value);
        }
    } else if (type == NVS_TYPE_U32) {
        uint32_t value = strtoul(str_value, NULL, 0);
        if (errno != ERANGE) {
            err = nvs_set_u32(nvs, key, value);
        }
    } else if (type == NVS_TYPE_I64) {
        int64_t value = strtoll(str_value, NULL, 0);
        if (errno != ERANGE) {
            err = nvs_set_i64(nvs, key, value);
        }
    } else if (type == NVS_TYPE_U64) {
        uint64_t value = strtoull(str_value, NULL, 0);
        if (errno != ERANGE) {
            err = nvs_set_u64(nvs, key, value);
        }
    } else if (type == NVS_TYPE_STR) {
        err = nvs_set_str(nvs, key, str_value);
    } else if (type == NVS_TYPE_BLOB) {
        err = store_blob(nvs, key, str_value);
    }

    if (range_error || errno == ERANGE) {
        nvs_close(nvs);
        return ESP_ERR_NVS_VALUE_TOO_LONG;
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Value stored under key '%s'", key);
        }
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t get_value_from_nvs(const char *key, const char *str_type)
{
    nvs_handle_t nvs;
    esp_err_t err;

    nvs_type_t type = nvs_backup_type_parse(str_type);

    if (type == NVS_TYPE_ANY) {
        ESP_LOGE(TAG, "Type '%s' is undefined", str_type);
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    err = nvs_open(current_namespace, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (type == NVS_TYPE_I8) {
        int8_t value;
        err = nvs_get_i8(nvs, key, &value);
        if (err == ESP_OK) {
            printf("%d\n", value);
        }
    } else if (type == NVS_TYPE_U8) {
        uint8_t value;
        err = nvs_get_u8(nvs, key, &value);
        if (err == ESP_OK) {
            printf("%u\n", value);
        }
    } else if (type == NVS_TYPE_I16) {
        int16_t value;
        err = nvs_get_i16(nvs, key, &value);
        if (err == ESP_OK) {
            printf("%u\n", value);
        }
    } else if (type == NVS_TYPE_U16) {
        uint16_t value;
        if ((err = nvs_get_u16(nvs, key, &value)) == ESP_OK) {
            printf("%u\n", value);
        }
    } else if (type == NVS_TYPE_I32) {
        int32_t value;
        if ((err = nvs_get_i32(nvs, key, &value)) == ESP_OK) {
            printf("%"PRIi32"\n", value);
        }
    } else if (type == NVS_TYPE_U32) {
        uint32_t value;
        if ((err = nvs_get_u32(nvs, key, &value)) == ESP_OK) {
            printf("%"PRIu32"\n", value);
        }
    } else if (type == NVS_TYPE_I64) {
        int64_t value;
        if ((err = nvs_get_i64(nvs, key, &value)) == ESP_OK) {
            printf("%lld\n", value);
        }
    } else if (type == NVS_TYPE_U64) {
        uint64_t value;
        if ( (err = nvs_get_u64(nvs, key, &value)) == ESP_OK) {
            printf("%llu\n", value);
        }
    } else if (type == NVS_TYPE_STR) {
        size_t len;
        if ( (err = nvs_get_str(nvs, key, NULL, &len)) == ESP_OK) {
            char *str = (char *)malloc(len);
            if ( (err = nvs_get_str(nvs, key, str, &len)) == ESP_OK) {
                printf("%s\n", str);
            }
            free(str);
        }
    } else if (type == NVS_TYPE_BLOB) {
        size_t len;
        if ( (err = nvs_get_blob(nvs, key, NULL, &len)) == ESP_OK) {
            char *blob = (char *)malloc(len);
            if ( (err = nvs_get_blob(nvs, key, blob, &len)) == ESP_OK) {
                print_blob(blob, len);
            }
            free(blob);
        }
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t erase(const char *key)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(current_namespace, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Value with key '%s' erased", key);
            }
        }
        nvs_close(nvs);
    }

    return err;
}

static esp_err_t erase_all(const char *name)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(name, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
    }

    ESP_LOGI(TAG, "Namespace '%s' was %s erased", name, (err == ESP_OK) ? "" : "not");

    nvs_close(nvs);
    return ESP_OK;
}

static int list(const char *part, const char *name, const char *str_type)
{
    nvs_type_t type = nvs_backup_type_parse(str_type);

    nvs_iterator_t it = NULL;
    esp_err_t result = nvs_entry_find(part, name[0] != '\0' ? name : NULL, type, &it);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "No such entry was found");
        return 1;
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS error: %s", esp_err_to_name(result));
        return 1;
    }

    do {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        result = nvs_entry_next(&it);

        printf("namespace '%s', key '%s', type '%s' \n",
               info.namespace_name, info.key, nvs_backup_type_name(info.type));
    } while (result == ESP_OK);

    if (result != ESP_ERR_NVS_NOT_FOUND) { // the last iteration ran into an internal error
        ESP_LOGE(TAG, "NVS error %s at current iteration, stopping.", esp_err_to_name(result));
        return 1;
    }

    return 0;
}

static int set_value(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_args.end, argv[0]);
        return 1;
    }

    const char *key = set_args.key->sval[0];
    const char *type = set_args.type->sval[0];
    const char *values = set_args.value->sval[0];

    esp_err_t err = set_value_in_nvs(key, type, values);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static int get_value(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &get_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, get_args.end, argv[0]);
        return 1;
    }

    const char *key = get_args.key->sval[0];
    const char *type = get_args.type->sval[0];

    esp_err_t err = get_value_from_nvs(key, type);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static int erase_value(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &erase_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, erase_args.end, argv[0]);
        return 1;
    }

    const char *key = erase_args.key->sval[0];

    esp_err_t err = erase(key);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static int erase_namespace(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &erase_all_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, erase_all_args.end, argv[0]);
        return 1;
    }

    const char *name = erase_all_args.namespace->sval[0];

    esp_err_t err = erase_all(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static int set_namespace(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &namespace_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, namespace_args.end, argv[0]);
        return 1;
    }

    const char *namespace = namespace_args.namespace->sval[0];
    strlcpy(current_namespace, namespace, sizeof(current_namespace));
    ESP_LOGI(TAG, "Namespace set to '%s'", current_namespace);
    return 0;
}

static int list_entries(int argc, char **argv)
{
    list_args.partition->sval[0] = "";
    list_args.namespace->sval[0] = "";
    list_args.type->sval[0] = "";

    int nerrors = arg_parse(argc, argv, (void **) &list_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, list_args.end, argv[0]);
        return 1;
    }

    const char *part = list_args.partition->sval[0];
    const char *name = list_args.namespace->sval[0];
    const char *type = list_args.type->sval[0];

    return list(part, name, type);
}

void register_nvs(void)
{
    set_args.key = arg_str1(NULL, NULL, "<key>", "key of the value to be set");
    set_args.type = arg_str1(NULL, NULL, "<type>", ARG_TYPE_STR);

    set_args.value = arg_str1("v", "value", "<value>", "value to be stored");
    set_args.end = arg_end(2);

    get_args.key = arg_str1(NULL, NULL, "<key>", "key of the value to be read");
    get_args.type = arg_str1(NULL, NULL, "<type>", ARG_TYPE_STR);
    get_args.end = arg_end(2);

    erase_args.key = arg_str1(NULL, NULL, "<key>", "key of the value to be erased");
    erase_args.end = arg_end(2);

    erase_all_args.namespace = arg_str1(NULL, NULL, "<namespace>", "namespace to be erased");
    erase_all_args.end = arg_end(2);

    namespace_args.namespace = arg_str1(NULL, NULL, "<namespace>", "namespace of the partition to be selected");
    namespace_args.end = arg_end(2);

    list_args.partition = arg_str1(NULL, NULL, "<partition>", "partition name");
    list_args.namespace = arg_str0("n", "namespace", "<namespace>", "namespace name");
    list_args.type = arg_str0("t", "type", "<type>", ARG_TYPE_STR);
    list_args.end = arg_end(2);

    const esp_console_cmd_t set_cmd = {
        .command = "nvs_set",
        .help = "Set key-value pair in selected namespace.\n"
        "Examples:\n"
        " nvs_set VarName i32 -v 123 \n"
        " nvs_set VarName str -v YourString \n"
        " nvs_set VarName blob -v 0123456789abcdef \n",
        .hint = NULL,
        .func = &set_value,
        .argtable = &set_args
    };

    const esp_console_cmd_t get_cmd = {
        .command = "nvs_get",
        .help = "Get key-value pair from selected namespace. \n"
        "Example: nvs_get VarName i32",
        .hint = NULL,
        .func = &get_value,
        .argtable = &get_args
    };

    const esp_console_cmd_t erase_cmd = {
        .command = "nvs_erase",
        .help = "Erase key-value pair from current namespace",
        .hint = NULL,
        .func = &erase_value,
        .argtable = &erase_args
    };

    const esp_console_cmd_t erase_namespace_cmd = {
        .command = "nvs_erase_namespace",
        .help = "Erases specified namespace",
        .hint = NULL,
        .func = &erase_namespace,
        .argtable = &erase_all_args
    };

    const esp_console_cmd_t namespace_cmd = {
        .command = "nvs_namespace",
        .help = "Set current namespace",
        .hint = NULL,
        .func = &set_namespace,
        .argtable = &namespace_args
    };

    const esp_console_cmd_t list_entries_cmd = {
        .command = "nvs_list",
        .help = "List stored key-value pairs stored in NVS."
        "Namespace and type can be specified to print only those key-value pairs.\n"
        "Following command list variables stored inside 'nvs' partition, under namespace 'storage' with type uint32_t"
        "Example: nvs_list nvs -n storage -t u32 \n",
        .hint = NULL,
        .func = &list_entries,
        .argtable = &list_args
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&set_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&get_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&erase_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&namespace_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_entries_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&erase_namespace_cmd));
}

/* ------------------------------------------------------------------ backup */

/*
 * Output in blocks with a breath between: the USB CDC console drops lines under a flood, and
 * telnet's stdout is unbuffered, so every write there is a send().
 */
#define PACE_BYTES 512

/* After a printf: a breath once PACE_BYTES have gone out since the last. */
static void pace(size_t *since, int n)
{
    if (n > 0) {
        *since += (size_t)n;
    }
    if (*since >= PACE_BYTES) {
        fflush(stdout);
        vTaskDelay(1);
        *since = 0;
    }
}

static void paced_write(const char *text, size_t len)
{
    for (size_t off = 0; off < len; off += PACE_BYTES) {
        const size_t n = len - off < PACE_BYTES ? len - off : PACE_BYTES;
        fwrite(text + off, 1, n, stdout);
        fflush(stdout);
        vTaskDelay(1);
    }
}

static struct {
    struct arg_lit *exclusions;
    struct arg_end *end;
} inventory_args;

static struct {
    struct arg_str *path;
    struct arg_end *end;
} export_args;

typedef struct {
    size_t paced;                           /* bytes printed since the last breath */
    unsigned in, out, namespaces;           /* the totals */
    char ns[NVS_NS_NAME_MAX_SIZE];          /* the namespace being listed */
    unsigned ns_in, ns_out;
} inventory_t;

/* The subtotal under a namespace's last entry. */
static void inventory_namespace_done(inventory_t *t)
{
    if (t->ns[0] != '\0') {
        pace(&t->paced, printf("  %s: %u included, %u excluded\n", t->ns, t->ns_in, t->ns_out));
        t->namespaces++;
    }
}

/* One pass: the entries arrive sorted, so a namespace's are together. */
static bool inventory_entry(const nvs_entry_info_t *e, const nvs_backup_exclusion_t *x, void *ctx)
{
    inventory_t *t = ctx;
    if (strcmp(t->ns, e->namespace_name) != 0) {
        inventory_namespace_done(t);
        strlcpy(t->ns, e->namespace_name, sizeof(t->ns));
        t->ns_in = t->ns_out = 0;
    }
    const char *type = nvs_backup_type_name(e->type);
    if (x == NULL) {
        t->ns_in++;
        t->in++;
        pace(&t->paced, printf("%-15s %-15s %-4s include\n", e->namespace_name, e->key, type));
    } else {
        t->ns_out++;
        t->out++;
        pace(&t->paced, printf("%-15s %-15s %-4s exclude  %s (%s)\n", e->namespace_name, e->key,
                               type, x->reason, x->kit ? "kit" : "app"));
    }
    return true;
}

static bool inventory_rule(const nvs_backup_exclusion_t *x, void *ctx)
{
    pace(ctx, printf("%-15s %-15s %-3s  %s\n", x->ns, x->key != NULL ? x->key : "*",
                     x->kit ? "kit" : "app", x->reason));
    return true;
}

static int nvs_inventory_cmd(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&inventory_args) != 0) {
        arg_print_errors(stderr, inventory_args.end, argv[0]);
        return 1;
    }
    inventory_t t = { 0 };
    if (inventory_args.exclusions->count > 0) {
        printf("%-15s %-15s %-3s  %s\n", "NAMESPACE", "KEY", "BY", "REASON");
        nvs_backup_exclusions(inventory_rule, &t.paced);
        fflush(stdout);
        return 0;
    }
    printf("%-15s %-15s %-4s %s\n", "NAMESPACE", "KEY", "TYPE", "VERDICT");
    nvs_stats_t st;
    const esp_err_t err = nvs_backup_inventory(inventory_entry, &t, &st);
    if (err != ESP_OK) {
        printf("nvs_inventory: %s\n", esp_err_to_name(err));
        return 1;
    }
    inventory_namespace_done(&t);
    printf("%u entries in %u namespaces: %u in a backup, %u excluded\n", t.in + t.out,
           t.namespaces, t.in, t.out);
    printf("partition: %u of %u slots used, %u available, %u namespaces\n",
           (unsigned)st.used_entries, (unsigned)st.total_entries,
           (unsigned)st.available_entries, (unsigned)st.namespace_count);
    fflush(stdout);
    return 0;
}

static int nvs_export_cmd(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&export_args) != 0) {
        arg_print_errors(stderr, export_args.end, argv[0]);
        return 1;
    }
    /* Resolve the path before the export, so a bad one costs nothing. */
    const bool to_file = export_args.path->count > 0;
    char abs[FS_ABS_MAX];
    char why[64];
    if (to_file) {
        const int perr = nvs_backup_path(export_args.path->sval[0], abs, sizeof(abs), why,
                                         sizeof(why));
        if (perr != 0) {
            printf("nvs_export: %s\n", why);
            return 1;
        }
    }
    char *text;
    size_t len;
    nvs_backup_export_info_t info;
    const esp_err_t err = nvs_backup_export(&text, &len, &info);
    if (err != ESP_OK) {
        printf("nvs_export: refused: %s\n", info.detail[0] ? info.detail : esp_err_to_name(err));
        return 1;
    }
    if (!to_file) {
        paced_write(text, len);
        free(text);
        return 0;
    }
    char sha[65];
    const int werr = nvs_backup_write_file(abs, text, len, sha);
    free(text);
    if (werr != 0) {
        printf("nvs_export: %s: %s\n", abs, strerror(werr));
        return 1;
    }
    printf("%s: %u bytes, %u entries (%u excluded), sha256 %s\n", abs, (unsigned)len,
           info.entries, info.excluded, sha);
    return 0;
}

void register_nvs_backup(const nvs_backup_config_t *cfg)
{
    nvs_backup_init(cfg);

    inventory_args.exclusions = arg_lit0("x", "exclusions", "list the exclusion rules instead");
    inventory_args.end = arg_end(1);
    export_args.path = arg_str0(NULL, NULL, "[path]",
                                "a file to write: a name goes in the backup directory, "
                                "a path from / is on the volume");
    export_args.end = arg_end(1);

    const esp_console_cmd_t inventory_cmd = {
        .command = "nvs_inventory",
        .help = "Every NVS entry, and whether a backup would hold it or why not, with a "
                "count per namespace and the partition's use. -x: the exclusion rules.",
        .hint = NULL,
        .func = &nvs_inventory_cmd,
        .argtable = &inventory_args,
    };
    const esp_console_cmd_t export_cmd = {
        .command = "nvs_export",
        .help = "The configuration as a .r2cfg file: every NVS entry the exclusion rules do "
                "not keep back (no credential is ever in it). With no path, printed; with "
                "one, written there and its size and SHA-256 printed.\n"
                "Examples:\n"
                " nvs_export\n"
                " nvs_export panel.r2cfg       (into the backup directory)\n"
                " nvs_export /backup/a.r2cfg   (a path on the volume)",
        .hint = NULL,
        .func = &nvs_export_cmd,
        .argtable = &export_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&inventory_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&export_cmd));
}
