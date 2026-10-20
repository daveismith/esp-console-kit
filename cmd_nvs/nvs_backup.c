/*
 * nvs_backup: the exclusion registry, the inventory and the export. See nvs_backup.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs_ops.h"
#include "nvs_backup.h"

/* The namespace the kit's wifi_known keeps its saved networks in (key "known"). Defined by
 * cmd_wifi's Kconfig when that component is in the build; its default otherwise, harmlessly. */
#ifdef CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE
#define WIFI_KNOWN_NS   CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE
#else
#define WIFI_KNOWN_NS   "wifi_known"
#endif

/* The trailer line: this, then the CRC-32 in 8 hex digits. */
#define CRC_PREFIX      "# crc32 "
#define CRC_PREFIX_LEN  8
#define CRC_LINE_LEN    (CRC_PREFIX_LEN + 8)

/* Large frames off the stack once they return: the snapshot and the writes run after them,
 * as deep as FAT and wear levelling go, on a console task of 6 KB. */
#define NOINLINE __attribute__((noinline))

#define BLOB_LINE_BYTES 64      /* 128 hex digits a continuation line */
#define TEXT_START      (8 * 1024)
#define SANE_TIME       1577836800  /* 2020-01-01: a clock before this was never set */

/* ------------------------------------------------------------------ the registry */

/*
 * The kit's defaults: what ESP-IDF and its drivers keep, and the credentials and identity
 * the kit's own components keep. Every namespace a driver owns is excluded whole, since what
 * it keeps there is its own business and changes between IDF versions.
 */
static const nvs_backup_exclusion_t s_kit[] = {
    { "phy", NULL, "per chip: RF calibration, regenerated at boot", true },
    { "nvs.net80211", NULL, "credential: the WiFi driver's own copy of its config", true },
    { "dhcp_state", NULL, "cache: lwIP's last DHCP lease", true },
    { "psa_its", NULL, "credential: PSA key storage", true },
    { "bt_config.conf", NULL, "identity: Bluetooth bonds", true },
    { "nimble_bond", NULL, "identity: Bluetooth bonds", true },
    { "bt_mesh*", NULL, "identity: Bluetooth mesh keys", true },
    { "misc", NULL, "identity: Bluetooth controller state", true },
    { WIFI_KNOWN_NS, "known", "credential: the saved WiFi networks", true },
    { "wifi_ap", NULL, "credential: the access point's name and passphrase", true },
    { "web", "password", "credential: the web password", true },
};
#define KIT_COUNT (sizeof(s_kit) / sizeof(s_kit[0]))

/* PSRAM where the build allows it: 256 bytes of internal DRAM is worth having back. */
static EXT_RAM_BSS_ATTR nvs_backup_exclusion_t s_app[NVS_BACKUP_APP_EXCLUSIONS];
static size_t s_app_count;

static nvs_backup_config_t s_cfg;

void nvs_backup_init(const nvs_backup_config_t *cfg)
{
    s_cfg = cfg != NULL ? *cfg : (nvs_backup_config_t){ 0 };
}

esp_err_t nvs_backup_exclude(const char *ns, const char *key, const char *reason)
{
    if (ns == NULL || ns[0] == '\0' || reason == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_app_count >= NVS_BACKUP_APP_EXCLUSIONS) {
        return ESP_ERR_NO_MEM;
    }
    s_app[s_app_count++] = (nvs_backup_exclusion_t){ ns, key, reason, false };
    return ESP_OK;
}

/* A name against a pattern: exact, or a prefix when the pattern ends in '*'. */
static bool name_matches(const char *pattern, const char *name)
{
    const size_t n = strlen(pattern);
    if (n > 0 && pattern[n - 1] == '*') {
        return strncmp(pattern, name, n - 1) == 0;
    }
    return strcmp(pattern, name) == 0;
}

static bool rule_matches(const nvs_backup_exclusion_t *x, const char *ns, const char *key)
{
    return name_matches(x->ns, ns) && (x->key == NULL || name_matches(x->key, key));
}

static const nvs_backup_exclusion_t *exclusion_for(const char *ns, const char *key)
{
    for (size_t i = 0; i < KIT_COUNT; i++) {
        if (rule_matches(&s_kit[i], ns, key)) {
            return &s_kit[i];
        }
    }
    for (size_t i = 0; i < s_app_count; i++) {
        if (rule_matches(&s_app[i], ns, key)) {
            return &s_app[i];
        }
    }
    return NULL;
}

bool nvs_backup_is_excluded(const char *ns, const char *key)
{
    return exclusion_for(ns, key) != NULL;
}

void nvs_backup_exclusions(bool (*cb)(const nvs_backup_exclusion_t *x, void *ctx), void *ctx)
{
    for (size_t i = 0; i < KIT_COUNT; i++) {
        if (!cb(&s_kit[i], ctx)) {
            return;
        }
    }
    for (size_t i = 0; i < s_app_count; i++) {
        if (!cb(&s_app[i], ctx)) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ memory */

/* PSRAM when there is any: these are the large, short-lived buffers. */
static void *big_realloc(void *p, size_t n)
{
    return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

void nvs_backup_wipe_free(void *p, size_t n)
{
    if (p != NULL) {
        memset(p, 0, n);
        free(p);
    }
}

/* ------------------------------------------------------------------ enumeration */

static int entry_cmp(const void *a, const void *b)
{
    const nvs_entry_info_t *x = a, *y = b;
    const int c = strcmp(x->namespace_name, y->namespace_name);
    return c != 0 ? c : strcmp(x->key, y->key);
}

static const char *part_name(void)
{
    return s_cfg.part != NULL ? s_cfg.part : NVS_DEFAULT_PART_NAME;
}

/*
 * Every entry of the partition into ONE array, sorted: one allocation rather than a node per
 * entry, because small mallocs stay in internal RAM. The caller frees *out.
 */
static esp_err_t collect(nvs_entry_info_t **out, size_t *count, nvs_stats_t *st)
{
    *out = NULL;
    *count = 0;
    size_t cap = 32;
    if (nvs_get_stats(part_name(), st) == ESP_OK && st->used_entries > cap) {
        cap = st->used_entries;     /* an upper bound: a blob or a string spans several */
    }
    nvs_entry_info_t *arr = big_realloc(NULL, cap * sizeof(*arr));
    if (arr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(part_name(), NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        if (n == cap) {             /* something was written since the stats were read */
            nvs_entry_info_t *grown = big_realloc(arr, cap * 2 * sizeof(*arr));
            if (grown == NULL) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            arr = grown;
            cap *= 2;
        }
        nvs_entry_info(it, &arr[n++]);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) {     /* NOT_FOUND is the end of the walk */
        free(arr);
        return err;
    }
    qsort(arr, n, sizeof(*arr), entry_cmp);
    /* A write on another task can move an item to a later page mid-walk, where the iterator
     * meets it again: keep one of each. */
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (kept == 0 || entry_cmp(&arr[kept - 1], &arr[i]) != 0) {
            arr[kept++] = arr[i];
        }
    }
    n = kept;
    *out = arr;
    *count = n;
    return ESP_OK;
}

esp_err_t nvs_backup_inventory(nvs_backup_visit_t cb, void *ctx, nvs_stats_t *stats)
{
    nvs_entry_info_t *arr;
    size_t n;
    nvs_stats_t st = { 0 };
    const esp_err_t err = collect(&arr, &n, &st);
    if (stats != NULL) {
        *stats = st;
    }
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < n; i++) {
        if (!cb(&arr[i], exclusion_for(arr[i].namespace_name, arr[i].key), ctx)) {
            break;
        }
    }
    free(arr);
    return ESP_OK;
}

/* ------------------------------------------------------------------ the text */

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} text_t;

/* Room for `n` more bytes and a NUL; NULL (and oom set) when there is none. */
static char *text_reserve(text_t *t, size_t n)
{
    if (t->oom) {
        return NULL;
    }
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : TEXT_START;
        while (t->len + n + 1 > cap) {
            cap *= 2;
        }
        /* Copied and wiped rather than realloc'd: an import's text may hold a refused
         * secret, and realloc would leave the old copy in freed memory. */
        char *grown = big_realloc(NULL, cap);
        if (grown == NULL) {
            t->oom = true;
            return NULL;
        }
        if (t->buf != NULL) {
            memcpy(grown, t->buf, t->len);
            nvs_backup_wipe_free(t->buf, t->cap);
        }
        t->buf = grown;
        t->cap = cap;
    }
    return t->buf + t->len;
}

static void text_printf(text_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void text_printf(text_t *t, const char *fmt, ...)
{
    if (t->oom) {
        return;
    }
    /* Into the room there is; only a line that does not fit is formatted twice. */
    const size_t room = t->cap - t->len;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(t->buf != NULL ? t->buf + t->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        t->oom = true;
        return;
    }
    if ((size_t)n >= room) {
        char *p = text_reserve(t, (size_t)n);
        if (p == NULL) {
            return;
        }
        va_start(ap, fmt);
        vsnprintf(p, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    t->len += (size_t)n;
}

static const char HEX[] = "0123456789abcdef";

static void text_blob(text_t *t, const uint8_t *v, size_t len)
{
    text_printf(t, " %u\n", (unsigned)len);
    for (size_t off = 0; off < len; off += BLOB_LINE_BYTES) {
        const size_t n = len - off < BLOB_LINE_BYTES ? len - off : BLOB_LINE_BYTES;
        char *p = text_reserve(t, 2 + 2 * n + 1);
        if (p == NULL) {
            return;
        }
        *p++ = '+';
        *p++ = ' ';
        for (size_t i = 0; i < n; i++) {
            *p++ = HEX[v[off + i] >> 4];
            *p++ = HEX[v[off + i] & 0xf];
        }
        *p++ = '\n';
        t->len = (size_t)(p - t->buf);
    }
}

/* One byte of a str as the format writes it -- \" \\ \n \xNN or itself -- into p (room for
 * ESC_MAX); returns the end. parse_str() is its inverse. */
#define ESC_MAX 4
static char *escape_char(char *p, unsigned char c)
{
    if (c == '"' || c == '\\') {
        *p++ = '\\';
        *p++ = (char)c;
    } else if (c == '\n') {
        *p++ = '\\';
        *p++ = 'n';
    } else if (c < 0x20 || c > 0x7e) {
        *p++ = '\\';
        *p++ = 'x';
        *p++ = HEX[c >> 4];
        *p++ = HEX[c & 0xf];
    } else {
        *p++ = (char)c;
    }
    return p;
}

static void text_str(text_t *t, const char *s)
{
    const size_t len = strlen(s);
    char *p = text_reserve(t, 2 + ESC_MAX * len + 2);   /* ' "', the bytes, '"\n' */
    if (p == NULL) {
        return;
    }
    *p++ = ' ';
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        p = escape_char(p, (unsigned char)s[i]);
    }
    *p++ = '"';
    *p++ = '\n';
    t->len = (size_t)(p - t->buf);
}

/* A query parameter whose name says it carries a secret: token, access_token, apiKey,
 * client_secret, password, X-Amz-Signature, auth, ... Broad on purpose: a false alarm refuses
 * an export and names the key; a miss puts a credential in a file. */
static bool secret_param(const char *name, size_t n)
{
    static const char *const words[] = { "token", "key", "pass", "pwd", "secret", "sig", "auth",
                                         "cred" };
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        const size_t wl = strlen(words[w]);
        for (size_t i = 0; i + wl <= n; i++) {
            if (strncasecmp(name + i, words[w], wl) == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * The credential backstop, over every URL in a string: anything before an '@' in the
 * authority -- scheme://user:pass@host, and scheme://token@host, a token as the user name
 * being a usual way to put one in a URL -- or a query parameter named like a secret
 * (?token=...). The registry is what keeps credentials out; this catches one slipped into an
 * ordinary setting. Blobs are opaque and not looked into.
 */
static bool url_with_credentials(const char *s)
{
    static const char URL_END[] = " \t\r\n\"'<>\\";
    for (const char *p = strstr(s, "://"); p != NULL; p = strstr(p, "://")) {
        p += 3;
        const size_t url = strcspn(p, URL_END);
        const size_t authority = strcspn(p, "/?#\"'<> \t\r\n\\");
        if (memchr(p, '@', authority) != NULL) {
            return true;
        }
        for (const char *q = p + authority; q < p + url; q++) {
            if (*q == '?' || *q == '&' || *q == ';') {
                const size_t n = strcspn(q + 1, "=&;# \t\r\n\"'<>\\");
                if (q[1 + n] == '=' && secret_param(q + 1, n)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* The format's type names, which are also the ones nvs_set and nvs_get take. */
static const struct {
    nvs_type_t type;
    const char *name;
} s_types[] = {
    { NVS_TYPE_U8, "u8" },   { NVS_TYPE_I8, "i8" },   { NVS_TYPE_U16, "u16" },
    { NVS_TYPE_I16, "i16" }, { NVS_TYPE_U32, "u32" }, { NVS_TYPE_I32, "i32" },
    { NVS_TYPE_U64, "u64" }, { NVS_TYPE_I64, "i64" }, { NVS_TYPE_STR, "str" },
    { NVS_TYPE_BLOB, "blob" },
};

const char *nvs_backup_type_name(nvs_type_t type)
{
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++) {
        if (s_types[i].type == type) {
            return s_types[i].name;
        }
    }
    return "?";
}

nvs_type_t nvs_backup_type_parse(const char *name)
{
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++) {
        if (strcmp(s_types[i].name, name) == 0) {
            return s_types[i].type;
        }
    }
    return NVS_TYPE_ANY;
}

/* An integer type's width in bytes and whether it is signed: NVS encodes both in the type. */
static bool is_int_type(nvs_type_t t)
{
    return t == NVS_TYPE_U8 || t == NVS_TYPE_I8 || t == NVS_TYPE_U16 || t == NVS_TYPE_I16 ||
           t == NVS_TYPE_U32 || t == NVS_TYPE_I32 || t == NVS_TYPE_U64 || t == NVS_TYPE_I64;
}
#define INT_BYTES(t)  ((unsigned)(t) & 0x0f)
#define INT_SIGNED(t) (((unsigned)(t) & 0x10) != 0)

/* An integer of any width, as an int64_t: sign-extended when signed, the bits when u64. */
static esp_err_t get_int(nvs_handle_t h, const char *key, nvs_type_t type, int64_t *v)
{
    esp_err_t err;
    switch (type) {
    case NVS_TYPE_U8:  { uint8_t x;  err = nvs_get_u8(h, key, &x);  *v = x; break; }
    case NVS_TYPE_U16: { uint16_t x; err = nvs_get_u16(h, key, &x); *v = x; break; }
    case NVS_TYPE_U32: { uint32_t x; err = nvs_get_u32(h, key, &x); *v = x; break; }
    case NVS_TYPE_U64: { uint64_t x; err = nvs_get_u64(h, key, &x); *v = (int64_t)x; break; }
    case NVS_TYPE_I8:  { int8_t x;   err = nvs_get_i8(h, key, &x);  *v = x; break; }
    case NVS_TYPE_I16: { int16_t x;  err = nvs_get_i16(h, key, &x); *v = x; break; }
    case NVS_TYPE_I32: { int32_t x;  err = nvs_get_i32(h, key, &x); *v = x; break; }
    case NVS_TYPE_I64: err = nvs_get_i64(h, key, v); break;
    default: return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    return err;
}

static esp_err_t set_int(nvs_handle_t h, const char *key, nvs_type_t type, int64_t v)
{
    switch (type) {
    case NVS_TYPE_U8:  return nvs_set_u8(h, key, (uint8_t)v);
    case NVS_TYPE_U16: return nvs_set_u16(h, key, (uint16_t)v);
    case NVS_TYPE_U32: return nvs_set_u32(h, key, (uint32_t)v);
    case NVS_TYPE_U64: return nvs_set_u64(h, key, (uint64_t)v);
    case NVS_TYPE_I8:  return nvs_set_i8(h, key, (int8_t)v);
    case NVS_TYPE_I16: return nvs_set_i16(h, key, (int16_t)v);
    case NVS_TYPE_I32: return nvs_set_i32(h, key, (int32_t)v);
    case NVS_TYPE_I64: return nvs_set_i64(h, key, v);
    default: return ESP_ERR_NVS_TYPE_MISMATCH;
    }
}

static int fmt_int(char *out, size_t size, nvs_type_t type, int64_t v)
{
    return INT_SIGNED(type) ? snprintf(out, size, "%" PRId64, v)
                            : snprintf(out, size, "%" PRIu64, (uint64_t)v);
}

/*
 * A str or blob into *scratch: one buffer for a whole export or import, read into straight
 * away and grown only when an entry does not fit (NVS then reports the size it needs).
 * `len` gets its size, a str's NUL included.
 */
#define SCRATCH_START 1024
typedef struct {
    uint8_t *buf;
    size_t cap;
} scratch_t;

static esp_err_t read_var(nvs_handle_t h, const char *key, nvs_type_t type, scratch_t *sc,
                          size_t *len)
{
    *len = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (sc->buf == NULL || *len > sc->cap) {
            const size_t want = *len > SCRATCH_START ? *len : SCRATCH_START;
            uint8_t *grown = big_realloc(sc->buf, want);
            if (grown == NULL) {
                return ESP_ERR_NO_MEM;
            }
            sc->buf = grown;
            sc->cap = want;
        }
        *len = sc->cap;
        const esp_err_t err = type == NVS_TYPE_STR ? nvs_get_str(h, key, (char *)sc->buf, len)
                                                   : nvs_get_blob(h, key, sc->buf, len);
        if (err != ESP_ERR_NVS_INVALID_LENGTH) {
            return err;
        }
    }
    return ESP_ERR_NVS_INVALID_LENGTH;   /* it grew between the two reads */
}

/* Zeroed before it is freed: it may hold a refused URL's password. */
static void scratch_free(scratch_t *sc)
{
    nvs_backup_wipe_free(sc->buf, sc->cap);
    *sc = (scratch_t){ 0 };
}

/* One included entry's line(s). ESP_ERR_NVS_NOT_FOUND when it went away since the walk. */
static esp_err_t write_entry(text_t *t, nvs_handle_t h, const nvs_entry_info_t *e,
                             scratch_t *sc, nvs_backup_export_info_t *info)
{
    const char *type = nvs_backup_type_name(e->type);
    if (is_int_type(e->type)) {
        int64_t v;
        const esp_err_t err = get_int(h, e->key, e->type, &v);
        if (err == ESP_OK) {
            char num[24];
            fmt_int(num, sizeof(num), e->type, v);
            text_printf(t, "%s %s %s\n", e->key, type, num);
        }
        return err;
    }
    if (e->type != NVS_TYPE_STR && e->type != NVS_TYPE_BLOB) {
        snprintf(info->detail, sizeof(info->detail), "%s/%s: unknown type 0x%02x",
                 e->namespace_name, e->key, (unsigned)e->type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    size_t len;
    const esp_err_t err = read_var(h, e->key, e->type, sc, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (e->type == NVS_TYPE_STR && url_with_credentials((const char *)sc->buf)) {
        snprintf(info->detail, sizeof(info->detail),
                 "%s/%s holds a URL with a credential in it (user, password or token)",
                 e->namespace_name, e->key);
        return ESP_ERR_NOT_ALLOWED;
    }
    text_printf(t, "%s %s", e->key, type);
    if (e->type == NVS_TYPE_STR) {
        text_str(t, (const char *)sc->buf);
    } else {
        text_blob(t, sc->buf, len);
    }
    return ESP_OK;
}

/*
 * A name the line format can carry unquoted: printable ASCII with no space, and nothing that
 * would read as another kind of line -- a key starting '#' (a comment), '+' (blob data) or
 * '[' (a section), a namespace holding '[' or ']'. NVS allows any bytes; none of this
 * firmware's names comes near.
 */
static bool name_ok(const char *name, bool is_key)
{
    if (is_key && (name[0] == '#' || name[0] == '+' || name[0] == '[')) {
        return false;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (*c <= ' ' || *c > '~' || (!is_key && (*c == '[' || *c == ']'))) {
            return false;
        }
    }
    return name[0] != '\0';
}

/* The included entries, a [namespace] line before each namespace's first. */
static esp_err_t write_body(text_t *t, const nvs_entry_info_t *arr, size_t n,
                            nvs_backup_export_info_t *info)
{
    scratch_t sc = { 0 };
    nvs_handle_t h = 0;
    const char *open_ns = NULL;     /* the namespace h is open on */
    const char *gone_ns = NULL;     /* one erased since the walk */
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < n && err == ESP_OK && !t->oom; i++) {
        const nvs_entry_info_t *e = &arr[i];
        if (nvs_backup_is_excluded(e->namespace_name, e->key)) {
            info->excluded++;       /* never read: a credential is never even in memory */
            continue;
        }
        if (gone_ns != NULL && strcmp(gone_ns, e->namespace_name) == 0) {
            continue;
        }
        if (!name_ok(e->namespace_name, false) || !name_ok(e->key, true)) {
            snprintf(info->detail, sizeof(info->detail),
                     "%s/%s: a name the file format cannot carry", e->namespace_name, e->key);
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (open_ns == NULL || strcmp(open_ns, e->namespace_name) != 0) {
            if (open_ns != NULL) {
                nvs_close(h);
                open_ns = NULL;
            }
            err = nvs_open_from_partition(part_name(), e->namespace_name, NVS_READONLY, &h);
            if (err == ESP_ERR_NVS_NOT_FOUND) {     /* erased since the walk: not in the file */
                gone_ns = e->namespace_name;
                err = ESP_OK;
                continue;
            }
            if (err != ESP_OK) {
                snprintf(info->detail, sizeof(info->detail), "%s: %s", e->namespace_name,
                         esp_err_to_name(err));
                break;
            }
            open_ns = e->namespace_name;
            text_printf(t, "[%s]\n", e->namespace_name);
        }
        err = write_entry(t, h, e, &sc, info);
        if (err == ESP_ERR_NVS_NOT_FOUND) {         /* erased since the walk: not in the file */
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK && info->detail[0] == '\0') {
            snprintf(info->detail, sizeof(info->detail), "%s/%s: %s", e->namespace_name,
                     e->key, esp_err_to_name(err));
        }
        if (err == ESP_OK) {
            info->entries++;
        }
    }
    if (open_ns != NULL) {
        nvs_close(h);
    }
    scratch_free(&sc);
    return err;
}

/*
 * The header, which needs the entry count and so is written after the body. Its fields are
 * bounded: the app description's strings are 32-byte arrays, so it fits HEADER_MAX.
 */
#define HEADER_MAX 208
static int format_header(char *out, size_t size, unsigned entries)
{
    uint8_t mac[6];
    char board[18] = "unknown";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(board, sizeof(board), MACSTR, MAC2STR(mac));
    }
    char when[24] = "unknown";
    const time_t now = time(NULL);
    struct tm tm;
    if (now >= SANE_TIME && gmtime_r(&now, &tm) != NULL) {
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    return snprintf(out, size,
                    "# r2cfg %d\n# board %s\n# fw %s idf %s project %s time %s\n# entries %u\n",
                    NVS_BACKUP_FORMAT, board, app->version, app->idf_ver, app->project_name,
                    when, entries);
}

esp_err_t nvs_backup_export(char **text, size_t *len, nvs_backup_export_info_t *info)
{
    *text = NULL;
    *len = 0;
    memset(info, 0, sizeof(*info));

    nvs_entry_info_t *arr;
    size_t n;
    nvs_stats_t st;
    esp_err_t err = collect(&arr, &n, &st);
    if (err != ESP_OK) {
        snprintf(info->detail, sizeof(info->detail), "walking the partition: %s",
                 esp_err_to_name(err));
        return err;
    }
    text_t t = { 0 };
    err = write_body(&t, arr, n, info);
    free(arr);
    if (err == ESP_OK) {
        /* The header in front of the body, in the same buffer; then the CRC of both. */
        char hdr[HEADER_MAX];
        const int hlen = format_header(hdr, sizeof(hdr), info->entries);
        char *p = hlen > 0 && hlen < (int)sizeof(hdr) ? text_reserve(&t, (size_t)hlen) : NULL;
        if (p != NULL) {
            memmove(t.buf + hlen, t.buf, t.len);
            memcpy(t.buf, hdr, (size_t)hlen);
            t.len += (size_t)hlen;
            const uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)t.buf, t.len);
            text_printf(&t, CRC_PREFIX "%08" PRIx32 "\n", crc);
        } else {
            t.oom = true;
        }
        if (t.oom) {
            err = ESP_ERR_NO_MEM;
            snprintf(info->detail, sizeof(info->detail), "out of memory for the text");
        }
    }
    if (err != ESP_OK) {
        free(t.buf);
        return err;
    }
    *text = t.buf;
    *len = t.len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ files */

int nvs_backup_path(const char *name, char *abs, size_t abs_len, char *why, size_t why_len)
{
    if (name[0] == '/') {
        return fs_path(name, abs, abs_len, why, why_len);
    }
    if (s_cfg.dir == NULL) {
        snprintf(why, why_len, "no backup directory is configured: give a path from /");
        return ENOTSUP;
    }
    char rel[FS_PATH_MAX + 16];
    const int w = snprintf(rel, sizeof(rel), "%s/%s", s_cfg.dir, name);
    if (w < 0 || (size_t)w >= sizeof(rel)) {
        snprintf(why, why_len, "path too long");
        return EINVAL;
    }
    return fs_path(rel, abs, abs_len, why, why_len);
}

int nvs_backup_write_file(const char *abs, const char *text, size_t len, char *sha256_hex)
{
    const char *slash = strrchr(abs, '/');
    if (slash != NULL && slash != abs) {
        char *dir = strndup(abs, (size_t)(slash - abs));
        if (dir == NULL) {
            return ENOMEM;
        }
        const int err = fs_mkdir(dir, true);
        free(dir);
        if (err != 0 && err != EEXIST) {
            return err;
        }
    }
    fs_writer_t *w;
    int err = fs_write_begin(abs, len, true, &w);
    if (err != 0) {
        return err;
    }
    err = fs_write(w, text, len);
    if (err != 0) {
        fs_write_abort(w);
        return err;
    }
    return fs_write_finish(w, NULL, sha256_hex, NULL);
}

esp_err_t nvs_backup_export_file(const char *abs, size_t *len, char *sha256_hex,
                                 nvs_backup_export_info_t *info)
{
    char *text;
    esp_err_t err = nvs_backup_export(&text, len, info);
    if (err != ESP_OK) {
        return err;
    }
    const int werr = nvs_backup_write_file(abs, text, *len, sha256_hex);
    free(text);
    if (werr != 0) {
        snprintf(info->detail, sizeof(info->detail), "%.70s: %s", abs, strerror(werr));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ import: the parse */

static const char *TAG = "nvs_backup";

#define STR_MAX 4000    /* NVS's own limit on a str, its NUL included */

typedef enum { ACT_SAME, ACT_CHANGE, ACT_ADD, ACT_RETYPE, ACT_EXCLUDED, ACT_REMOVE } act_t;

/* One entry of the file, or (ACT_REMOVE) one of the partition's that --replace erases. */
typedef struct {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    uint8_t act;
    unsigned line;
    int64_t v;          /* an integer, as get_int() gives it */
    size_t off, len;    /* a str or blob: its bytes in `data`; a str's NUL follows them */
} item_t;

typedef struct {
    item_t *items;
    size_t n, cap;
    uint8_t *data;                      /* the decoded strs and blobs */
    size_t dlen, dcap;
    bool board_known;                   /* the header names a board, and it is `board` */
    uint8_t board[6];
    char fw[32];                        /* the header's firmware version */
    long entries;                       /* the header's count; -1 when it has none */
} parsed_t;

static void parsed_free(parsed_t *p)
{
    free(p->items);
    nvs_backup_wipe_free(p->data, p->dcap);     /* a skipped excluded entry may be a secret */
}

/* Room for one more element in a growing array (PSRAM). */
static bool grow(void **arr, size_t *cap, size_t n, size_t each)
{
    if (n < *cap) {
        return true;
    }
    const size_t want = *cap != 0 ? *cap * 2 : 32;
    void *p = big_realloc(*arr, want * each);
    if (p == NULL) {
        return false;
    }
    *arr = p;
    *cap = want;
    return true;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Decimal digits filling all of [s, s+n), and nothing else. */
static bool parse_u64(const char *s, size_t n, uint64_t *out)
{
    if (n == 0) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        const unsigned d = (unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* An integer of `type` into *v, as get_int() gives it; false when out of range. */
static bool parse_int(const char *s, size_t n, nvs_type_t type, int64_t *v)
{
    const unsigned bits = INT_BYTES(type) * 8;
    const bool neg = INT_SIGNED(type) && n > 0 && s[0] == '-';
    uint64_t mag;
    if (!parse_u64(s + neg, n - neg, &mag)) {
        return false;
    }
    if (!INT_SIGNED(type)) {
        if (bits < 64 && mag > (1ULL << bits) - 1) {
            return false;
        }
        *v = (int64_t)mag;
        return true;
    }
    const uint64_t lim = 1ULL << (bits - 1);    /* -lim .. lim-1 */
    if (neg ? mag > lim : mag >= lim) {
        return false;
    }
    *v = neg ? (mag == 0 ? 0 : -(int64_t)(mag - 1) - 1) : (int64_t)mag;
    return true;
}

/* A name of [s, s+n) into out (NVS's 16 bytes), checked as the export checks it. */
static bool take_name(const char *s, size_t n, bool is_key, char out[NVS_KEY_NAME_MAX_SIZE])
{
    if (n == 0 || n >= NVS_KEY_NAME_MAX_SIZE) {
        return false;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return name_ok(out, is_key);
}

/* A quoted str, [s, s+n) quotes included, decoded into p->data with its NUL. */
static const char *parse_str(const char *s, size_t n, parsed_t *p, item_t *it)
{
    if (n < 2 || s[0] != '"' || s[n - 1] != '"') {
        return "a str is quoted";
    }
    it->off = p->dlen;
    for (size_t i = 1; i < n - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            return "an unescaped '\"' in a str";
        }
        if (c < 0x20 || c > 0x7e) {
            return "a str holds a raw control or non-ASCII byte (\\xNN escapes it)";
        }
        if (c == '\\') {
            if (i + 1 >= n - 1) {
                return "a str ends in a lone '\\'";
            }
            const char e = s[++i];
            if (e == '"' || e == '\\') {
                c = (unsigned char)e;
            } else if (e == 'n') {
                c = '\n';
            } else if (e == 'x' && i + 2 < n - 1 && hexval(s[i + 1]) >= 0 &&
                       hexval(s[i + 2]) >= 0) {
                c = (unsigned char)(hexval(s[i + 1]) << 4 | hexval(s[i + 2]));
                i += 2;
                if (c == 0) {
                    return "a str cannot hold a NUL";
                }
            } else {
                return "an unknown escape in a str (\\\" \\\\ \\n \\xNN)";
            }
        }
        p->data[p->dlen++] = c;     /* decoded is never longer than encoded: it fits */
    }
    it->len = p->dlen - it->off;
    p->data[p->dlen++] = '\0';
    return it->len + 1 > STR_MAX ? "a str of more than 3999 bytes" : NULL;
}

/* The header lines this format knows; any other comment is ignored. */
static const char *parse_comment(const char *s, size_t n, parsed_t *p)
{
    if (n > 8 && memcmp(s, "# board ", 8) == 0) {
        s += 8;
        n -= 8;
        if (n == 7 && memcmp(s, "unknown", 7) == 0) {
            p->board_known = false;
            return NULL;
        }
        if (n != 17) {
            return "a # board line is a MAC, aa:bb:cc:dd:ee:ff";
        }
        for (int i = 0; i < 6; i++) {
            const int hi = hexval(s[3 * i]), lo = hexval(s[3 * i + 1]);
            if (hi < 0 || lo < 0 || (i < 5 && s[3 * i + 2] != ':')) {
                return "a # board line is a MAC, aa:bb:cc:dd:ee:ff";
            }
            p->board[i] = (uint8_t)(hi << 4 | lo);
        }
        p->board_known = true;
    } else if (n > 5 && memcmp(s, "# fw ", 5) == 0) {
        const char *sp = memchr(s + 5, ' ', n - 5);
        const size_t fl = (sp != NULL ? (size_t)(sp - s) : n) - 5;
        snprintf(p->fw, sizeof(p->fw), "%.*s", (int)fl, s + 5);
    } else if (n > 10 && memcmp(s, "# entries ", 10) == 0) {
        uint64_t v;
        if (!parse_u64(s + 10, n - 10, &v) || v > NVS_BACKUP_MAX_FILE) {
            return "a # entries line is a count";
        }
        p->entries = (long)v;
    }
    return NULL;
}

/* One "key type value" line into a new item; a blob's bytes follow on '+' lines. */
static const char *parse_entry(const char *s, size_t n, const char *ns, unsigned lineno,
                               parsed_t *p)
{
    const char *sp1 = memchr(s, ' ', n);
    const char *sp2 = sp1 != NULL ? memchr(sp1 + 1, ' ', n - (size_t)(sp1 + 1 - s)) : NULL;
    if (sp2 == NULL) {
        return "an entry is \"key type value\"";
    }
    if (!grow((void **)&p->items, &p->cap, p->n, sizeof(*p->items))) {
        return "out of memory";
    }
    item_t *it = &p->items[p->n];
    *it = (item_t){ .line = lineno };
    if (!take_name(s, (size_t)(sp1 - s), true, it->key)) {
        return "a key of 1 to 15 characters, printable, no space, not starting # + [";
    }
    strlcpy(it->ns, ns, sizeof(it->ns));
    char tname[8];
    const size_t tl = (size_t)(sp2 - sp1 - 1);
    if (tl == 0 || tl >= sizeof(tname)) {
        return "an unknown type";
    }
    memcpy(tname, sp1 + 1, tl);
    tname[tl] = '\0';
    it->type = nvs_backup_type_parse(tname);
    const char *v = sp2 + 1;
    const size_t vn = n - (size_t)(v - s);
    if (it->type == NVS_TYPE_ANY) {
        return "an unknown type";
    }
    const char *why = NULL;
    if (it->type == NVS_TYPE_STR) {
        why = parse_str(v, vn, p, it);
    } else if (it->type == NVS_TYPE_BLOB) {
        uint64_t len = 0;
        if (!parse_u64(v, vn, &len) || len > NVS_BACKUP_MAX_FILE) {
            why = "a blob's size";
        }
        it->off = p->dlen;
        it->len = (size_t)len;  /* the bytes owed; the '+' lines fill them */
    } else if (!parse_int(v, vn, it->type, &it->v)) {
        why = "a decimal integer in the type's range";
    }
    if (why == NULL) {
        p->n++;
    }
    return why;
}

/*
 * The body (everything before the `# crc32` line) into `p`. ESP_ERR_INVALID_ARG or
 * ESP_ERR_INVALID_VERSION with r->detail naming the line, or ESP_ERR_NO_MEM.
 */
static NOINLINE esp_err_t parse(const char *text, size_t len, parsed_t *p,
                                nvs_backup_report_t *r)
{
    p->entries = -1;
    p->dcap = len + 1;
    p->data = big_realloc(NULL, p->dcap);
    if (p->data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char ns[NVS_NS_NAME_MAX_SIZE] = "";
    long blob = -1;         /* the item whose '+' lines are still owed */
    size_t have = 0;        /* of its bytes */
    unsigned lineno = 0;
    const char *why = NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (size_t pos = 0; pos < len && why == NULL; ) {
        const char *s = text + pos;
        const char *nl = memchr(s, '\n', len - pos);
        const size_t n = nl != NULL ? (size_t)(nl - s) : len - pos;
        pos += n + 1;
        lineno++;
        if (lineno == 1) {
            uint64_t format;
            if (n < 9 || memcmp(s, "# r2cfg ", 8) != 0 || !parse_u64(s + 8, n - 8, &format)) {
                why = "not a .r2cfg file: the first line is not \"# r2cfg <format>\"";
            } else if (format != NVS_BACKUP_FORMAT) {
                why = "a format this firmware does not read";
                err = ESP_ERR_INVALID_VERSION;
            }
            continue;
        }
        if (n > 0 && s[0] == '+') {
            if (blob < 0) {
                why = "blob data with no blob before it";
                continue;
            }
            item_t *it = &p->items[blob];
            const size_t hex = n - 2;
            if (n < 4 || s[1] != ' ' || hex % 2 != 0) {
                why = "a blob line is \"+ \" and an even number of hex digits";
                continue;
            }
            if (have + hex / 2 > it->len) {
                why = "more blob data than its size says";
                continue;
            }
            for (size_t i = 0; i < hex && why == NULL; i += 2) {
                const int hi = hexval(s[2 + i]), lo = hexval(s[3 + i]);
                if (hi < 0 || lo < 0) {
                    why = "a blob line holds a character that is not hex";
                } else {
                    p->data[p->dlen++] = (uint8_t)(hi << 4 | lo);
                }
            }
            have += hex / 2;
            if (have == it->len) {
                blob = -1;
            }
            continue;
        }
        if (blob >= 0) {
            why = "a blob ends short of its size";
            lineno--;   /* the blob's last line is the one to blame */
            continue;
        }
        if (n == 0) {
            continue;
        }
        if (s[0] == '#') {
            why = parse_comment(s, n, p);
        } else if (s[0] == '[') {
            if (n < 3 || s[n - 1] != ']' || !take_name(s + 1, n - 2, false, ns)) {
                why = "a section is [namespace], 1 to 15 characters, printable, no space";
            }
        } else if (ns[0] == '\0') {
            why = "an entry before any [namespace]";
        } else {
            why = parse_entry(s, n, ns, lineno, p);
            if (why == NULL && p->items[p->n - 1].type == NVS_TYPE_BLOB) {
                blob = p->items[p->n - 1].len > 0 ? (long)(p->n - 1) : -1;
                have = 0;
            }
        }
    }
    if (why == NULL && blob >= 0) {
        why = "the file ends inside a blob";
    }
    if (why == NULL && lineno == 0) {
        why = "an empty file";
    }
    if (why != NULL) {
        snprintf(r->detail, sizeof(r->detail), "line %u: %s", lineno, why);
        return err;
    }
    if (p->entries < 0) {
        snprintf(r->detail, sizeof(r->detail), "no \"# entries\" line in the header");
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)p->entries != p->n) {
        snprintf(r->detail, sizeof(r->detail), "the header says %ld entries, the file holds %u",
                 p->entries, (unsigned)p->n);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static int item_cmp(const void *a, const void *b)
{
    const item_t *x = a, *y = b;
    const int c = strcmp(x->ns, y->ns);
    return c != 0 ? c : strcmp(x->key, y->key);
}

/* The trailer: the last line is "# crc32 xxxxxxxx", over every byte before it. */

static esp_err_t check_crc(const char *text, size_t len, size_t *body, nvs_backup_report_t *r)
{
    size_t end = len;
    if (end > 0 && text[end - 1] == '\n') {
        end--;
    }
    size_t start = end;
    while (start > 0 && text[start - 1] != '\n') {
        start--;
    }
    uint32_t want = 0;
    bool ok = end - start == CRC_LINE_LEN && memcmp(text + start, CRC_PREFIX, CRC_PREFIX_LEN) == 0;
    for (size_t i = start + CRC_PREFIX_LEN; ok && i < end; i++) {
        const int d = hexval(text[i]);
        ok = d >= 0;
        want = want << 4 | (uint32_t)(d & 0xf);
    }
    if (!ok) {
        snprintf(r->detail, sizeof(r->detail),
                 "no \"# crc32\" line at the end: truncated, or not a .r2cfg file");
        return ESP_ERR_INVALID_CRC;
    }
    const uint32_t got = esp_rom_crc32_le(0, (const uint8_t *)text, start);
    if (got != want) {
        snprintf(r->detail, sizeof(r->detail),
                 "CRC mismatch: the file says %08" PRIx32 ", its bytes give %08" PRIx32
                 " (damaged, edited, or \\r\\n line endings)", want, got);
        return ESP_ERR_INVALID_CRC;
    }
    *body = start;
    return ESP_OK;
}

/* ------------------------------------------------------------------ import: the diff */

#define SAY_MAX 192

static void say(nvs_backup_line_t line, void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void say(nvs_backup_line_t line, void *ctx, const char *fmt, ...)
{
    if (line == NULL) {
        return;
    }
    char buf[SAY_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    line(buf, ctx);
}

/* A value as a diff line shows it: an integer, a str quoted and cut short, a blob's size. */
#define PREVIEW_MAX 52
static void preview(char *out, size_t size, nvs_type_t type, int64_t v, const uint8_t *b,
                    size_t len)
{
    if (is_int_type(type)) {
        fmt_int(out, size, type, v);
    } else if (type == NVS_TYPE_BLOB) {
        snprintf(out, size, "%u bytes", (unsigned)len);
    } else if (url_with_credentials((const char *)b)) {
        snprintf(out, size, "(a URL with a credential in it: not shown)");
    } else {
        /* Quoted as the file has it, cut short with "..." when it does not fit. */
        char *o = out, *const end = out + size - (ESC_MAX + 5);    /* '"' ESC '..."' NUL */
        *o++ = '"';
        for (size_t i = 0; b[i] != '\0'; i++) {
            if (o >= end) {
                memcpy(o, "...", 3);
                o += 3;
                break;
            }
            o = escape_char(o, b[i]);
        }
        *o++ = '"';
        *o = '\0';
    }
}

/*
 * Each entry against the partition: same, change, add or retype. Reads only; an excluded
 * entry is not looked at, here or anywhere.
 */
static NOINLINE esp_err_t diff(parsed_t *p, nvs_backup_report_t *r, nvs_backup_line_t line,
                               void *ctx)
{
    scratch_t sc = { 0 };
    nvs_handle_t h = 0;
    const char *open_ns = NULL;
    bool ns_exists = false;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < p->n && err == ESP_OK; i++) {
        item_t *it = &p->items[i];
        if (it->act == ACT_EXCLUDED) {
            continue;
        }
        if (open_ns == NULL || strcmp(open_ns, it->ns) != 0) {
            if (ns_exists) {
                nvs_close(h);
            }
            open_ns = it->ns;
            err = nvs_open_from_partition(part_name(), it->ns, NVS_READONLY, &h);
            ns_exists = err == ESP_OK;
            if (err == ESP_ERR_NVS_NOT_FOUND) {     /* a namespace this board does not have */
                err = ESP_OK;
            }
            if (err != ESP_OK) {
                snprintf(r->detail, sizeof(r->detail), "%s: %s", it->ns, esp_err_to_name(err));
                break;
            }
        }
        /* Read it as the file's type: NOT_FOUND is an add, TYPE_MISMATCH a retype (asking
         * its old type only then), anything else is a value to compare. */
        bool same = false;
        size_t len = 0;
        int64_t v = 0;
        if (!ns_exists) {
            err = ESP_ERR_NVS_NOT_FOUND;
        } else if (is_int_type(it->type)) {
            err = get_int(h, it->key, it->type, &v);
            same = err == ESP_OK && v == it->v;
        } else {
            err = read_var(h, it->key, it->type, &sc, &len);
            const size_t want = it->type == NVS_TYPE_STR ? it->len + 1 : it->len;
            same = err == ESP_OK && len == want && memcmp(sc.buf, p->data + it->off, want) == 0;
        }
        nvs_type_t had = it->type;
        if (err == ESP_ERR_NVS_TYPE_MISMATCH && nvs_find_key(h, it->key, &had) == ESP_OK &&
            had != it->type) {
            err = ESP_OK;
        }
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            snprintf(r->detail, sizeof(r->detail), "reading %s/%s: %s", it->ns, it->key,
                     esp_err_to_name(err));
            break;
        }
        if (same) {
            it->act = ACT_SAME;
            r->same++;
            say(line, ctx, "same     %s/%s", it->ns, it->key);
            continue;
        }
        char nv[PREVIEW_MAX];
        preview(nv, sizeof(nv), it->type, it->v, p->data + it->off, it->len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
            it->act = ACT_ADD;
            r->added++;
            say(line, ctx, "add      %s/%s %s %s", it->ns, it->key,
                nvs_backup_type_name(it->type), nv);
        } else if (had != it->type) {
            it->act = ACT_RETYPE;
            r->retyped++;
            say(line, ctx, "retype   %s/%s: %s -> %s %s", it->ns, it->key,
                nvs_backup_type_name(had), nvs_backup_type_name(it->type), nv);
        } else if (it->type == NVS_TYPE_BLOB && len == it->len) {
            it->act = ACT_CHANGE;
            r->changed++;
            unsigned differ = 0;
            for (size_t b = 0; b < len; b++) {
                differ += sc.buf[b] != p->data[it->off + b];
            }
            say(line, ctx, "change   %s/%s: %s, %u of them differ", it->ns, it->key, nv, differ);
        } else {
            it->act = ACT_CHANGE;
            r->changed++;
            char ov[PREVIEW_MAX];
            preview(ov, sizeof(ov), it->type, v, sc.buf, len);
            say(line, ctx, "change   %s/%s: %s -> %s", it->ns, it->key, ov, nv);
        }
    }
    if (ns_exists) {
        nvs_close(h);
    }
    scratch_free(&sc);
    return err;
}

static int item_ns_cmp(const void *key, const void *elem)
{
    return strcmp((const char *)key, ((const item_t *)elem)->ns);
}

/* Whether the file manages namespace `ns`: it holds an entry there that is not excluded. A
 * namespace whose only entries were skipped by --ignore-excluded is not the file's to clear. */
static bool file_has_ns(const parsed_t *p, size_t nfile, const char *ns)
{
    const item_t *hit = bsearch(ns, p->items, nfile, sizeof(*p->items), item_ns_cmp);
    if (hit == NULL) {
        return false;
    }
    const item_t *first = p->items, *end = p->items + nfile;
    while (hit > first && strcmp(hit[-1].ns, ns) == 0) {
        hit--;
    }
    for (; hit < end && strcmp(hit->ns, ns) == 0; hit++) {
        if (hit->act != ACT_EXCLUDED) {
            return true;
        }
    }
    return false;
}

/* --replace: every entry of the file's namespaces that the file does not hold and the
 * registry does not keep back, appended as ACT_REMOVE and told. One walk of the partition,
 * collect()'s, already sorted and with an item met twice kept once. */
static NOINLINE esp_err_t find_removals(parsed_t *p, nvs_backup_report_t *r,
                                        nvs_backup_line_t line, void *ctx)
{
    nvs_entry_info_t *arr;
    size_t n;
    nvs_stats_t st;
    esp_err_t err = collect(&arr, &n, &st);
    if (err != ESP_OK) {
        snprintf(r->detail, sizeof(r->detail), "walking the partition: %s", esp_err_to_name(err));
        return err;
    }
    const size_t nfile = p->n;
    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        const nvs_entry_info_t *e = &arr[i];
        item_t probe = { .type = e->type, .act = ACT_REMOVE };
        strlcpy(probe.ns, e->namespace_name, sizeof(probe.ns));
        strlcpy(probe.key, e->key, sizeof(probe.key));
        if (nvs_backup_is_excluded(probe.ns, probe.key) ||
            !file_has_ns(p, nfile, probe.ns) ||
            bsearch(&probe, p->items, nfile, sizeof(*p->items), item_cmp) != NULL) {
            continue;       /* kept back, a namespace the file lacks, or in the file */
        }
        if (!grow((void **)&p->items, &p->cap, p->n, sizeof(*p->items))) {
            err = ESP_ERR_NO_MEM;
            snprintf(r->detail, sizeof(r->detail), "out of memory");
            break;
        }
        p->items[p->n++] = probe;
        r->removed++;
        say(line, ctx, "remove   %s/%s %s", probe.ns, probe.key, nvs_backup_type_name(probe.type));
    }
    free(arr);
    /* Back into one order, so a namespace's writes and erases share one commit. */
    qsort(p->items, p->n, sizeof(*p->items), item_cmp);
    return err;
}

/* NVS slots a write takes: one for an integer; a str or blob its header, index and data. */
static size_t slots_for(const item_t *it)
{
    if (is_int_type(it->type)) {
        return 1;
    }
    const size_t bytes = it->type == NVS_TYPE_STR ? it->len + 1 : it->len;
    return 2 + (bytes + 31) / 32;
}

/* What the import sets; and what it sets or erases. */
static bool sets(const item_t *it)
{
    return it->act == ACT_CHANGE || it->act == ACT_ADD || it->act == ACT_RETYPE;
}

static bool writes(const item_t *it)
{
    return sets(it) || it->act == ACT_REMOVE;
}

/* Everything that can refuse the file, in order, then the diff. Reads only. */
static NOINLINE esp_err_t check(parsed_t *p, const nvs_backup_import_opts_t *o,
                                nvs_backup_report_t *r, nvs_backup_line_t line, void *ctx)
{
    r->entries = (unsigned)p->n;
    qsort(p->items, p->n, sizeof(*p->items), item_cmp);
    for (size_t i = 1; i < p->n; i++) {
        if (item_cmp(&p->items[i - 1], &p->items[i]) == 0) {
            snprintf(r->detail, sizeof(r->detail), "%s/%s is in the file twice (lines %u and %u)",
                     p->items[i].ns, p->items[i].key, p->items[i - 1].line, p->items[i].line);
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint8_t mac[6];
    const bool mine = p->board_known && esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK &&
                      memcmp(mac, p->board, sizeof(mac)) == 0;
    if (!mine && !o->other_board) {
        if (p->board_known) {
            snprintf(r->detail, sizeof(r->detail),
                     "the file is board " MACSTR "'s, not this one's: --other-board imports it",
                     MAC2STR(p->board));
        } else {
            snprintf(r->detail, sizeof(r->detail),
                     "the file names no board: --other-board imports it");
        }
        return ESP_ERR_INVALID_MAC;
    }
    if (!mine) {
        say(line, ctx, "note     another board's file, imported with --other-board");
    }
    const esp_app_desc_t *app = esp_app_get_description();
    if (p->fw[0] != '\0' && strcmp(p->fw, app->version) != 0) {
        say(line, ctx, "note     written by firmware %s, this is %s", p->fw, app->version);
    }

    const item_t *first = NULL;
    for (size_t i = 0; i < p->n; i++) {
        item_t *it = &p->items[i];
        const nvs_backup_exclusion_t *x = exclusion_for(it->ns, it->key);
        if (x != NULL) {
            it->act = ACT_EXCLUDED;
            r->excluded++;
            first = first != NULL ? first : it;
            say(line, ctx, "%s %s/%s: %s", o->ignore_excluded ? "skip    " : "excluded",
                it->ns, it->key, x->reason);
        }
    }
    if (first != NULL && !o->ignore_excluded) {
        snprintf(r->detail, sizeof(r->detail),
                 "%u excluded entr%s, %s/%s the first: --ignore-excluded skips them",
                 r->excluded, r->excluded == 1 ? "y" : "ies", first->ns, first->key);
        return ESP_ERR_NOT_ALLOWED;
    }

    esp_err_t err = diff(p, r, line, ctx);
    if (err == ESP_OK && o->replace) {
        err = find_removals(p, r, line, ctx);
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t need = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (sets(&p->items[i])) {
            need += slots_for(&p->items[i]);
        }
    }
    nvs_stats_t st;
    if (need > 0 && nvs_get_stats(part_name(), &st) == ESP_OK && need > st.available_entries) {
        snprintf(r->detail, sizeof(r->detail), "NVS has %u free slots, the import needs about %u",
                 (unsigned)st.available_entries, (unsigned)need);
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ import: the writes */

/* NVS_BACKUP_SNAPSHOT: the configuration as it is, before the first change. Its own frame,
 * so the path and the export's bookkeeping are off the stack when the writes run. */
static NOINLINE esp_err_t snapshot(nvs_backup_report_t *r)
{
    if (s_cfg.dir == NULL) {
        return ESP_OK;      /* stdout only: there is nowhere to put one */
    }
    char abs[FS_ABS_MAX];
    char why[64];
    if (nvs_backup_path(NVS_BACKUP_SNAPSHOT, abs, sizeof(abs), why, sizeof(why)) != 0) {
        snprintf(r->detail, sizeof(r->detail),
                 "the pre-import snapshot: %s (--no-snapshot goes without)", why);
        return ESP_FAIL;
    }
    /* The previous snapshot moves aside rather than being overwritten: when it is the file
     * being imported -- the undo of a failed import -- it is the only copy of the state
     * before, until this import succeeds. */
    char prev[FS_ABS_MAX];
    if (nvs_backup_path(NVS_BACKUP_SNAPSHOT_PREV, prev, sizeof(prev), why, sizeof(why)) == 0) {
        fs_move(abs, prev, true);   /* ENOENT on a first import, which is fine */
    }
    size_t len;
    nvs_backup_export_info_t info;
    const esp_err_t err = nvs_backup_export_file(abs, &len, NULL, &info);
    if (err != ESP_OK) {
        snprintf(r->detail, sizeof(r->detail),
                 "the pre-import snapshot failed: %.64s (--no-snapshot goes without)",
                 info.detail[0] != '\0' ? info.detail : esp_err_to_name(err));
        return err;
    }
    r->snapshot = true;
    return ESP_OK;
}

/* The changes, one commit per namespace. An excluded entry is skipped again here: nothing
 * reaches nvs_set or nvs_erase without the registry having been asked at this point. */
static esp_err_t apply(const parsed_t *p, nvs_backup_report_t *r)
{
    nvs_handle_t h = 0;
    const char *open_ns = NULL;
    const item_t *it = NULL;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < p->n && err == ESP_OK; i++) {
        it = &p->items[i];
        if (!writes(it) || nvs_backup_is_excluded(it->ns, it->key)) {
            continue;
        }
        if (open_ns == NULL || strcmp(open_ns, it->ns) != 0) {
            if (open_ns != NULL) {
                err = nvs_commit(h);
                nvs_close(h);
                if (err != ESP_OK) {
                    snprintf(r->detail, sizeof(r->detail), "committing %s: %s", open_ns,
                             esp_err_to_name(err));
                    return err;
                }
                open_ns = NULL;
            }
            r->started = true;      /* opening for write can create the namespace */
            err = nvs_open_from_partition(part_name(), it->ns, NVS_READWRITE, &h);
            if (err != ESP_OK) {
                break;
            }
            open_ns = it->ns;
        }
        if (it->act == ACT_REMOVE || it->act == ACT_RETYPE) {
            err = nvs_erase_key(h, it->key);
            if (err == ESP_OK) {
                r->written++;
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;       /* gone already */
            }
            if (err != ESP_OK || it->act == ACT_REMOVE) {
                continue;
            }
        }
        const uint8_t *b = p->data + it->off;
        err = is_int_type(it->type)     ? set_int(h, it->key, it->type, it->v)
            : it->type == NVS_TYPE_STR  ? nvs_set_str(h, it->key, (const char *)b)
                                        : nvs_set_blob(h, it->key, b, it->len);
        if (err == ESP_OK) {
            r->written++;
        }
    }
    if (err != ESP_OK) {
        snprintf(r->detail, sizeof(r->detail), "writing %s/%s: %s", it->ns, it->key,
                 esp_err_to_name(err));
    }
    if (open_ns != NULL) {
        const esp_err_t c = nvs_commit(h);    /* what was written before a failure, too */
        nvs_close(h);
        if (err == ESP_OK && c != ESP_OK) {
            snprintf(r->detail, sizeof(r->detail), "committing %s: %s", open_ns,
                     esp_err_to_name(c));
            err = c;
        }
    }
    return err;
}

esp_err_t nvs_backup_import(const char *text, size_t len, const nvs_backup_import_opts_t *o,
                            nvs_backup_report_t *r, nvs_backup_line_t line, void *ctx)
{
    memset(r, 0, sizeof(*r));
    if (len > NVS_BACKUP_MAX_FILE) {
        snprintf(r->detail, sizeof(r->detail), "%u bytes: a backup is at most %u",
                 (unsigned)len, (unsigned)NVS_BACKUP_MAX_FILE);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t body;
    esp_err_t err = check_crc(text, len, &body, r);
    if (err != ESP_OK) {
        return err;
    }
    parsed_t p = { 0 };
    err = parse(text, body, &p, r);
    if (err == ESP_OK) {
        err = check(&p, o, r, line, ctx);
    }
    const unsigned todo = r->changed + r->added + r->retyped + r->removed;
    if (err == ESP_OK && !o->dry_run && todo > 0) {
        err = o->no_snapshot ? ESP_OK : snapshot(r);
        if (err == ESP_OK) {
            err = apply(&p, r);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "configuration imported, %u writes: restart required", r->written);
            } else {
                ESP_LOGE(TAG, "configuration import failed after %u writes: %s", r->written,
                         r->detail);
            }
        }
    }
    parsed_free(&p);
    return err;
}

/* ------------------------------------------------------------------ import: the sources */

int nvs_backup_read_file(const char *abs, char **text, size_t *len, char *why, size_t why_len)
{
    *text = NULL;
    *len = 0;
    struct stat st;
    if (stat(abs, &st) != 0) {
        const int e = errno;
        snprintf(why, why_len, "%s", strerror(e));
        return e;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(why, why_len, "not a file");
        return EISDIR;
    }
    if (st.st_size > NVS_BACKUP_MAX_FILE) {
        snprintf(why, why_len, "%ld bytes: a backup is at most %u", (long)st.st_size,
                 (unsigned)NVS_BACKUP_MAX_FILE);
        return EFBIG;
    }
    const size_t size = (size_t)st.st_size;
    char *buf = big_realloc(NULL, size + 1);
    if (buf == NULL) {
        snprintf(why, why_len, "out of memory");
        return ENOMEM;
    }
    FILE *f = fopen(abs, "rb");
    const size_t got = f != NULL ? fread(buf, 1, size, f) : 0;
    const int e = f == NULL ? errno : ferror(f) ? EIO : 0;
    if (f != NULL) {
        fclose(f);
    }
    if (e != 0 || got != size) {
        free(buf);
        snprintf(why, why_len, "reading: %s", strerror(e != 0 ? e : EIO));
        return e != 0 ? e : EIO;
    }
    buf[size] = '\0';
    *text = buf;
    *len = size;
    return 0;
}

#define TRAILER_DRAIN_MS 100

esp_err_t nvs_backup_read_stdin(const char *ready, int idle_ms, char **text, size_t *len,
                                char *why, size_t why_len)
{
    *text = NULL;
    *len = 0;
    const char *no = s_cfg.stdin_refusal != NULL ? s_cfg.stdin_refusal() : NULL;
    if (no != NULL) {
        snprintf(why, why_len, "%s", no);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ready != NULL) {
        printf("%s\n", ready);
        fflush(stdout);
    }
    /* Non-blocking, so silence can be timed; the console's own mode comes back after. */
    const int fd = fileno(stdin);
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    text_t t = { 0 };
    esp_err_t err = ESP_OK;     /* or what went wrong, the input still drained to its end */
    size_t col = 0;             /* bytes on the line so far */
    char head[CRC_PREFIX_LEN];  /* and the first of them */
    bool done = false;
    TickType_t last = xTaskGetTickCount();
    while (!done) {
        /* A byte at a time, so nothing after the trailer is taken from the console. */
        char c;
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                snprintf(why, why_len, "reading stdin: %s", strerror(errno));
                err = ESP_FAIL;
                break;
            }
            if (xTaskGetTickCount() - last >= pdMS_TO_TICKS(idle_ms)) {
                snprintf(why, why_len, "%d s with no input%s", idle_ms / 1000,
                         t.len > 0 ? ", and no \"# crc32\" line: truncated" : "");
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last = xTaskGetTickCount();
        if (c == '\r' || (c == '\n' && col == 0)) {
            continue;                   /* a "\r\n" sent, or a '\r' the console made '\n' */
        }
        if (c != '\n') {
            if (col < CRC_PREFIX_LEN) {
                head[col] = c;
            }
            col++;
        }
        /* The trailer ends the file as soon as it is whole: its newline may follow or not,
         * and anything after it is the console's again. */
        done = col == CRC_LINE_LEN && memcmp(head, CRC_PREFIX, CRC_PREFIX_LEN) == 0;
        if (c == '\n') {
            col = 0;
        }
        if (err != ESP_OK) {
            continue;                   /* refused already: drain to the end */
        }
        if (t.len + 2 > NVS_BACKUP_MAX_FILE) {
            snprintf(why, why_len, "more than %u bytes", (unsigned)NVS_BACKUP_MAX_FILE);
            err = ESP_ERR_INVALID_SIZE;
        } else if (text_reserve(&t, 2) == NULL) {
            snprintf(why, why_len, "out of memory");
            err = ESP_ERR_NO_MEM;
        } else {
            t.buf[t.len++] = c;
            if (done) {
                t.buf[t.len++] = '\n';
            }
            t.buf[t.len] = '\0';
        }
    }
    /* The trailer's own line ending, which would otherwise reach the console's line reader
     * as an empty line or two, and an extra prompt a scripted host would mistake for the
     * next command's. Only CR and LF, and only briefly: a host waits for the report and
     * prompt before it types again. */
    for (TickType_t quiet = xTaskGetTickCount(); done; ) {
        char c;
        if (read(fd, &c, 1) == 1) {
            if (c != '\r' && c != '\n') {
                break;
            }
            quiet = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - quiet >= pdMS_TO_TICKS(TRAILER_DRAIN_MS)) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags);
    }
    if (err != ESP_OK) {
        nvs_backup_wipe_free(t.buf, t.cap);
        return err;
    }
    *text = t.buf;
    *len = t.len;
    return ESP_OK;
}

esp_err_t nvs_backup_fetch(const char *url, char **text, size_t *len, char *why, size_t why_len)
{
    *text = NULL;
    *len = 0;
    why[0] = '\0';
    if (s_cfg.fetch == NULL) {
        snprintf(why, why_len, "this build fetches no URLs: import a file, or stdin");
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = s_cfg.fetch(url, NVS_BACKUP_MAX_FILE, text, len, why, why_len);
    if (err == ESP_OK && *len > NVS_BACKUP_MAX_FILE) {
        nvs_backup_wipe_free(*text, *len);
        *text = NULL;
        *len = 0;
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK && why[0] == '\0') {
        snprintf(why, why_len, "%s", esp_err_to_name(err));
    }
    return err;
}

const char *nvs_backup_import_note(void)
{
    return s_cfg.import_note != NULL ? s_cfg.import_note() : NULL;
}
