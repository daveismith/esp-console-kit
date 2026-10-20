/*
 * nvs_backup: the exclusion registry, the inventory and the export. See nvs_backup.h.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "fs_ops.h"
#include "nvs_backup.h"

/* The namespace the kit's wifi_known keeps its saved networks in. Defined by cmd_wifi's
 * Kconfig when that component is in the build; its default otherwise, harmlessly. */
#ifndef CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE
#define CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE "wifi_known"
#endif

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
    { CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE, "known", "credential: the saved WiFi networks", true },
    { "wifi_ap", NULL, "credential: the access point's name and passphrase", true },
    { "web", "password", "credential: the web password", true },
};
#define KIT_COUNT (sizeof(s_kit) / sizeof(s_kit[0]))

/* PSRAM where the build allows it: 256 bytes of internal DRAM is worth having back. */
static EXT_RAM_BSS_ATTR nvs_backup_exclusion_t s_app[NVS_BACKUP_APP_EXCLUSIONS];
static size_t s_app_count;

static const char *s_part;  /* NULL: the default partition */
static const char *s_dir;

void nvs_backup_init(const nvs_backup_config_t *cfg)
{
    s_part = cfg != NULL ? cfg->part : NULL;
    s_dir = cfg != NULL ? cfg->dir : NULL;
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

/* ------------------------------------------------------------------ enumeration */

static int entry_cmp(const void *a, const void *b)
{
    const nvs_entry_info_t *x = a, *y = b;
    const int c = strcmp(x->namespace_name, y->namespace_name);
    return c != 0 ? c : strcmp(x->key, y->key);
}

static const char *part_name(void)
{
    return s_part != NULL ? s_part : NVS_DEFAULT_PART_NAME;
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
        char *grown = big_realloc(t->buf, cap);
        if (grown == NULL) {
            t->oom = true;
            return NULL;
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

static void text_str(text_t *t, const char *s)
{
    const size_t len = strlen(s);
    char *p = text_reserve(t, 2 + 4 * len + 2);     /* ' "', \xNN at worst, '"\n' */
    if (p == NULL) {
        return;
    }
    *p++ = ' ';
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
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

/*
 * A str or blob into *scratch: one buffer for the whole export, read into straight away and
 * grown only when an entry does not fit (NVS then reports the size it needs).
 */
#define SCRATCH_START 1024
static esp_err_t read_var(nvs_handle_t h, const nvs_entry_info_t *e, uint8_t **scratch,
                          size_t *cap, size_t *len)
{
    const bool str = e->type == NVS_TYPE_STR;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (*scratch == NULL || *len > *cap) {
            const size_t want = *len > SCRATCH_START ? *len : SCRATCH_START;
            uint8_t *grown = big_realloc(*scratch, want);
            if (grown == NULL) {
                return ESP_ERR_NO_MEM;
            }
            *scratch = grown;
            *cap = want;
        }
        *len = *cap;
        const esp_err_t err = str ? nvs_get_str(h, e->key, (char *)*scratch, len)
                                  : nvs_get_blob(h, e->key, *scratch, len);
        if (err != ESP_ERR_NVS_INVALID_LENGTH) {
            return err;
        }
    }
    return ESP_ERR_NVS_INVALID_LENGTH;   /* it grew between the two reads */
}

/* One included entry's line(s). ESP_ERR_NVS_NOT_FOUND when it went away since the walk. */
static esp_err_t write_entry(text_t *t, nvs_handle_t h, const nvs_entry_info_t *e,
                             uint8_t **scratch, size_t *cap, nvs_backup_export_info_t *info)
{
    uint64_t u = 0;     /* an unsigned integer, widened */
    int64_t s = 0;      /* a signed one */
    bool is_signed = false;
    size_t len = 0;
    esp_err_t err;
    switch (e->type) {
    case NVS_TYPE_U8:  { uint8_t v;  err = nvs_get_u8(h, e->key, &v);  u = v; break; }
    case NVS_TYPE_U16: { uint16_t v; err = nvs_get_u16(h, e->key, &v); u = v; break; }
    case NVS_TYPE_U32: { uint32_t v; err = nvs_get_u32(h, e->key, &v); u = v; break; }
    case NVS_TYPE_U64: err = nvs_get_u64(h, e->key, &u); break;
    case NVS_TYPE_I8:  { int8_t v;  err = nvs_get_i8(h, e->key, &v);  s = v; is_signed = true; break; }
    case NVS_TYPE_I16: { int16_t v; err = nvs_get_i16(h, e->key, &v); s = v; is_signed = true; break; }
    case NVS_TYPE_I32: { int32_t v; err = nvs_get_i32(h, e->key, &v); s = v; is_signed = true; break; }
    case NVS_TYPE_I64: err = nvs_get_i64(h, e->key, &s); is_signed = true; break;
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: err = read_var(h, e, scratch, cap, &len); break;
    default:
        snprintf(info->detail, sizeof(info->detail), "%s/%s: unknown type 0x%02x",
                 e->namespace_name, e->key, (unsigned)e->type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        return err;
    }
    const char *type = nvs_backup_type_name(e->type);
    if (e->type == NVS_TYPE_STR && url_with_credentials((const char *)*scratch)) {
        snprintf(info->detail, sizeof(info->detail),
                 "%s/%s holds a URL with a credential in it (user, password or token)",
                 e->namespace_name, e->key);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (e->type == NVS_TYPE_STR || e->type == NVS_TYPE_BLOB) {
        text_printf(t, "%s %s", e->key, type);
        if (e->type == NVS_TYPE_STR) {
            text_str(t, (const char *)*scratch);
        } else {
            text_blob(t, *scratch, len);
        }
    } else if (is_signed) {
        text_printf(t, "%s %s %" PRId64 "\n", e->key, type, s);
    } else {
        text_printf(t, "%s %s %" PRIu64 "\n", e->key, type, u);
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
    uint8_t *scratch = NULL;
    size_t cap = 0;
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
        err = write_entry(t, h, e, &scratch, &cap, info);
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
    if (scratch != NULL) {
        memset(scratch, 0, cap);    /* it may hold the refused URL's password */
    }
    free(scratch);
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
            text_printf(&t, "# crc32 %08" PRIx32 "\n", crc);
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
    if (s_dir == NULL) {
        snprintf(why, why_len, "no backup directory is configured: give a path from /");
        return ENOTSUP;
    }
    char rel[FS_PATH_MAX + 16];
    const int w = snprintf(rel, sizeof(rel), "%s/%s", s_dir, name);
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
