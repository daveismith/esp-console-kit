/*
 * nvs_backup -- a board's configuration as a text file: every NVS entry, whatever its
 * namespace, key or type, except what the exclusion registry keeps back.
 *
 * Nothing here names a setting. The engine walks the partition with nvs_entry_find(), so a
 * key or namespace added anywhere in the firmware is in the next backup with no change here.
 * The one list anyone maintains is the exclusion registry: the kit seeds it with ESP-IDF's
 * and the drivers' internals and with the credentials and identity the kit's own components
 * keep (the saved WiFi networks, the access point, the web password), and an application
 * adds its own at boot with nvs_backup_exclude().
 *
 * THE CREDENTIAL RULE: whoever adds an NVS entry holding a credential -- a passphrase, a
 * password, a key, a token, a PMK -- excludes it in the same change, with the reason
 * "credential". An excluded entry is never read by the export, so it is never in memory
 * here either. As a backstop the export refuses outright, naming the key, when a `str`
 * holds a URL with a credential in it: a user or password (scheme://user:pass@host) or a
 * query parameter named like a secret (?token=, ?api_key=, ?X-Amz-Signature=, ...).
 *
 * The file (`.r2cfg`, format 1) is line-oriented text:
 *
 *     # r2cfg 1
 *     # board 34:85:18:aa:bb:cc
 *     # fw <version> idf <version> project <name> time 2026-10-19T14:02:11Z
 *     # entries 23
 *     [r2net]
 *     autojoin u8 1
 *     ipv4 blob 76
 *     + 0100000057...          (at most 64 bytes, 128 hex digits, per line)
 *     [leds]
 *     pull_url str "https://example.com/clips/"
 *     # crc32 8f3a21c0
 *
 * Namespaces and keys are sorted, so two exports of the same configuration differ only in
 * the time. Integers are decimal; a `str` is quoted with \" \\ \n and \xNN (exactly two hex
 * digits) for anything else outside printable ASCII; a blob is opaque bytes, its own version
 * field and its loader's migration being the owner's business. The trailer is zlib's CRC-32
 * (esp_rom_crc32_le(0, ...)) of every byte before the `# crc32` line, newlines being "\n".
 * The time is UTC, or `unknown` when the clock is before 2020.
 *
 * INTERNAL STACKS ONLY: the inventory and the export read NVS and nvs_backup_write_file()
 * writes flash, and either disables the cache, which a task on a PSRAM stack cannot survive.
 * The console and telnet tasks qualify; a caller on a PSRAM stack (the ROS task, for one)
 * hops to an internal one first. Large buffers are PSRAM when there is PSRAM, one allocation
 * each rather than one per entry; the stack use is small enough for a 6 KB console task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_BACKUP_FORMAT 1

/*
 * A URL fetcher. The kit fetches nothing itself: the application supplies one over its own
 * HTTP client. It returns a body of at most `max_bytes` that the caller frees with free(),
 * or an error with `why`; it must not write flash from a PSRAM stack (nor need to).
 */
typedef esp_err_t (*nvs_backup_fetch_t)(const char *url, size_t max_bytes, char **body,
                                        size_t *len, char *why, size_t why_len);

typedef struct {
    const char *part;   /* NULL: NVS_DEFAULT_PART_NAME ("nvs") */
    const char *dir;    /* where `nvs_export <name>` writes ("/data/backup"); NULL: stdout only */
    /* Optional. Why stdin cannot carry a file right now, or NULL when it can: a console
     * whose stdin is not the caller's says so here. NULL: stdin is always taken to be the
     * console's. */
    const char *(*stdin_refusal)(void);
    /* Optional. `nvs_import <url>`'s download; NULL: no URL imports. */
    nvs_backup_fetch_t fetch;
    /* Optional. A line `nvs_import` prints after a successful import, or NULL for none: the
     * application's reminder of what a backup never holds (its WiFi networks, say). */
    const char *(*import_note)(void);
} nvs_backup_config_t;

/* One rule of the registry. `ns` and `key` may end in '*', a prefix match; `key` NULL means
 * every key of the namespace. */
typedef struct {
    const char *ns;
    const char *key;
    const char *reason; /* "credential: ...", "identity: ...", "counter: ...", ... */
    bool kit;           /* a kit default, rather than the application's */
} nvs_backup_exclusion_t;

/*
 * Keep ns/key out of every backup. The strings are kept by pointer, so they must outlive the
 * registry (string literals). Call at boot, before anything exports. ESP_ERR_NO_MEM when the
 * application's slots (NVS_BACKUP_APP_EXCLUSIONS) are used up, ESP_ERR_INVALID_ARG for a NULL
 * namespace or reason.
 */
#define NVS_BACKUP_APP_EXCLUSIONS 16
esp_err_t nvs_backup_exclude(const char *ns, const char *key, const char *reason);
bool nvs_backup_is_excluded(const char *ns, const char *key);
/* Every rule, the kit's then the application's; `cb` returns false to stop. */
void nvs_backup_exclusions(bool (*cb)(const nvs_backup_exclusion_t *x, void *ctx), void *ctx);

/*
 * Every entry of the partition, sorted by namespace then key, with the rule that excludes it
 * (NULL when it is included). `cb` returns false to stop. `stats` (optional) gets the
 * partition's nvs_get_stats(), all zero if it could not be read.
 */
typedef bool (*nvs_backup_visit_t)(const nvs_entry_info_t *e, const nvs_backup_exclusion_t *x,
                                   void *ctx);
esp_err_t nvs_backup_inventory(nvs_backup_visit_t cb, void *ctx, nvs_stats_t *stats);

/* A type as the format names it -- "u8" .. "i64", "str", "blob" -- or "?"; and back, with
 * NVS_TYPE_ANY for a name that is none of them. nvs_set and nvs_get take the same names. */
const char *nvs_backup_type_name(nvs_type_t type);
nvs_type_t nvs_backup_type_parse(const char *name);

typedef struct {
    unsigned entries;   /* in the file */
    unsigned excluded;  /* kept back by the registry */
    char detail[96];    /* on a failure: why, naming the entry */
} nvs_backup_export_info_t;

/*
 * Every included entry as a format-1 file, in one buffer (PSRAM when there is any) that the
 * caller frees with free(). Refused, with nothing returned and `info->detail` naming the
 * entry: ESP_ERR_NOT_ALLOWED when a `str` holds a URL with a credential in it;
 * ESP_ERR_NOT_SUPPORTED for a type or a name the format cannot carry (a space, a key that
 * starts '#', '+' or '[').
 */
esp_err_t nvs_backup_export(char **text, size_t *len, nvs_backup_export_info_t *info);

/*
 * `name` as a path on the volume: a bare name is put in the configured directory; one that
 * starts with '/' is relative to the volume ("/backup/a.r2cfg") or absolute in it
 * ("/data/backup/a.r2cfg"). Checked as fs_path() checks; 0 or an errno value, with `why`.
 */
int nvs_backup_path(const char *name, char *abs, size_t abs_len, char *why, size_t why_len);

/*
 * Write `text` to `abs` through fs_write_begin()/fs_write_finish() (a .part and a rename, so
 * a failed write costs nothing), making its directory if need be. `sha256_hex` (65 bytes,
 * optional) gets the file's SHA-256. 0 or an errno value. Writes flash: internal stack only.
 */
int nvs_backup_write_file(const char *abs, const char *text, size_t len, char *sha256_hex);

/* nvs_backup_export() into `abs` through nvs_backup_write_file(): `len` and `sha256_hex`
 * (optional) describe the file. ESP_FAIL with info->detail when the write fails. */
esp_err_t nvs_backup_export_file(const char *abs, size_t *len, char *sha256_hex,
                                 nvs_backup_export_info_t *info);

/* Zero `n` bytes at `p`, then free() it: for a buffer that may have held a secret. */
void nvs_backup_wipe_free(void *p, size_t n);

/* The partition, directory and hooks to use (copied). register_nvs_backup() calls it. */
void nvs_backup_init(const nvs_backup_config_t *cfg);

/* ------------------------------------------------------------------ import */

#define NVS_BACKUP_MAX_FILE  (64 * 1024)        /* the largest file any source may supply */
#define NVS_BACKUP_SNAPSHOT  "pre-import.r2cfg" /* in the configured directory */
#define NVS_BACKUP_SNAPSHOT_PREV "pre-import.1.r2cfg" /* the one before, moved aside */

typedef struct {
    bool dry_run;           /* check and print the diff; write nothing */
    bool replace;           /* also erase keys of the file's namespaces that it does not hold */
    bool ignore_excluded;   /* skip excluded entries in the file, rather than refuse it */
    bool other_board;       /* accept a file whose header names another board */
    bool no_snapshot;       /* write no NVS_BACKUP_SNAPSHOT first: for when one cannot be
                             * written (no volume, or a setting the export refuses) */
} nvs_backup_import_opts_t;

typedef struct {
    unsigned entries;       /* in the file */
    unsigned same, changed, added, retyped, removed, excluded;
    unsigned written;       /* NVS sets and erases made: every one, or those before a failure */
    bool started;           /* writing began, so a failure left the partition part-imported */
    bool snapshot;          /* NVS_BACKUP_SNAPSHOT was written before the first change */
    char detail[128];       /* on a refusal or a failure: why, naming the line or entry */
} nvs_backup_report_t;

/* One line of the diff or of a refusal's list, without a newline. */
typedef void (*nvs_backup_line_t)(const char *line, void *ctx);

/*
 * Import a format-1 file (`opts` required). The whole file is checked before anything is
 * written: its CRC, its header and entry count, every line's syntax, names, types, values
 * and sizes, the board it came from, the excluded entries it holds and the partition's free
 * space; then the diff is computed and told, a line per entry (same, change, add, retype,
 * remove, excluded/skip), through `line` (optional). Unless it is a dry run, and only when
 * something differs: NVS_BACKUP_SNAPSHOT is written first (an export of the configuration
 * as it was, when a directory is configured), then the changes, one commit per namespace.
 * Unchanged values are not written. An excluded entry is never written, nor erased by `replace`.
 *
 * Refused with nothing written (`report->detail` says why): ESP_ERR_INVALID_CRC (a bad or
 * missing trailer), ESP_ERR_INVALID_VERSION (a newer format), ESP_ERR_INVALID_ARG (a line
 * that does not parse, a name or value out of range, a count that does not match),
 * ESP_ERR_INVALID_MAC (another board's file, without other_board), ESP_ERR_NOT_ALLOWED
 * (excluded entries, without ignore_excluded), ESP_ERR_NVS_NOT_ENOUGH_SPACE, or the
 * snapshot's own failure (no_snapshot proceeds without one). The snapshot before it is
 * kept as NVS_BACKUP_SNAPSHOT_PREV. A flash error part-way returns that error with
 * `started` set and `written` counting what was done; the snapshot holds the state before.
 *
 * A module that keeps a RAM copy of its settings may write the old state back over the
 * import, so the board wants a restart after one. Reads and writes flash: internal stack.
 */
esp_err_t nvs_backup_import(const char *text, size_t len, const nvs_backup_import_opts_t *opts,
                            nvs_backup_report_t *report, nvs_backup_line_t line, void *ctx);

/*
 * The three sources of a file. Each gives a buffer of at most NVS_BACKUP_MAX_FILE bytes
 * (PSRAM when there is any) that the caller frees with free(); on a failure, nothing, and
 * `why` says what went wrong.
 */

/* A file on the volume (`abs` from nvs_backup_path()). 0 or an errno value. Internal stack. */
int nvs_backup_read_file(const char *abs, char **text, size_t *len, char *why, size_t why_len);

/*
 * stdin, read raw (no echo, no line editing) until the `# crc32` line, or refused after
 * `idle_ms` with no input. The configured stdin_refusal is asked first; then `ready`
 * (optional) is printed, and a host waits for it before sending, so a refusal never leaves
 * the file for the console's line reader to run as commands. '\r' and empty lines are
 * dropped, so a host sending "\r\n", or a console turning '\r' into '\n', delivers the "\n"
 * file the CRC covers. ESP_OK, ESP_ERR_NOT_SUPPORTED (refused, nothing read),
 * ESP_ERR_TIMEOUT, ESP_ERR_INVALID_SIZE (over the cap: the input is still drained to its
 * end) or ESP_ERR_NO_MEM.
 */
esp_err_t nvs_backup_read_stdin(const char *ready, int idle_ms, char **text, size_t *len,
                                char *why, size_t why_len);

/* A URL, through the configured fetcher (ESP_ERR_NOT_SUPPORTED without one). */
esp_err_t nvs_backup_fetch(const char *url, char **text, size_t *len, char *why, size_t why_len);

/* The configured import note, or NULL. */
const char *nvs_backup_import_note(void);

#ifdef __cplusplus
}
#endif
