/*
 * fs_ops: the file operations `fs` and the HTTP API share. See fs_ops.h.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "psa/crypto.h"
#include "fs_ops.h"

#define IO_CHUNK     (16 * 1024)
/* Left free after a write: LittleFS needs spare blocks to stay writable. */
#define SPACE_MARGIN (32 * 1024)
/* A modification time before this is a clock that was never set. */
#define SANE_TIME    1577836800     /* 2020-01-01 */

static char s_base[64];
static fs_info_fn_t s_info;
static void *s_info_ctx;
static fs_change_hook_t s_hook;
static void *s_hook_ctx;

void fs_ops_init(const char *base_path, fs_info_fn_t info, void *info_ctx)
{
    strlcpy(s_base, base_path, sizeof(s_base));
    size_t n = strlen(s_base);
    while (n > 1 && s_base[n - 1] == '/') {
        s_base[--n] = '\0';
    }
    s_info = info;
    s_info_ctx = info_ctx;
}

const char *fs_root(void)
{
    return s_base;
}

void fs_ops_set_change_hook(fs_change_hook_t hook, void *ctx)
{
    s_hook = hook;
    s_hook_ctx = ctx;
}

static void changing(const char *abs)
{
    if (s_hook != NULL) {
        s_hook(abs, s_hook_ctx);
    }
}

static bool is_root(const char *abs)
{
    return strcmp(abs, s_base) == 0;
}

/* ------------------------------------------------------------------ paths */

int fs_path(const char *rel, char *abs, size_t abs_len, char *why, size_t why_len)
{
    const size_t base_len = strlen(s_base);
    if (base_len == 0) {
        snprintf(why, why_len, "no volume");
        return EINVAL;
    }
    if (rel == NULL || rel[0] != '/') {
        snprintf(why, why_len, "a path starts with /, from the root of the volume: /clips/intro.mov");
        return EINVAL;
    }
    if (strncmp(rel, s_base, base_len) == 0 && (rel[base_len] == '/' || rel[base_len] == '\0')) {
        rel += base_len;            /* "/data/clips" is "/clips" */
    }
    if (strlen(rel) >= FS_PATH_MAX) {
        snprintf(why, why_len, "a path may be at most %d bytes", FS_PATH_MAX - 1);
        return EINVAL;
    }
    /* Each segment: not empty (bar a trailing /), not . or .., no control characters or \ */
    const char *p = rel;
    while (*p == '/') {
        if (p[1] == '\0') {
            break;                  /* the root, or a trailing / */
        }
        const char *seg = p + 1;
        const size_t n = strcspn(seg, "/");
        if (n == 0) {
            snprintf(why, why_len, "empty segment (//) in a path: %s", rel);
            return EINVAL;
        }
        if ((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) {
            snprintf(why, why_len, "'%.*s' is not allowed in a path: %s", (int)n, seg, rel);
            return EINVAL;
        }
        if (n > FS_NAME_MAX) {
            snprintf(why, why_len, "a name may be at most %d bytes", FS_NAME_MAX);
            return EINVAL;
        }
        for (size_t i = 0; i < n; i++) {
            if ((unsigned char)seg[i] < 0x20 || seg[i] == 0x7f || seg[i] == '\\') {
                snprintf(why, why_len, "control characters and \\ are not allowed in a path");
                return EINVAL;
            }
        }
        p = seg + n;
    }
    const int w = snprintf(abs, abs_len, "%s%s", s_base, rel);
    if (w < 0 || (size_t)w >= abs_len) {
        snprintf(why, why_len, "path too long");
        return EINVAL;
    }
    size_t len = strlen(abs);
    while (len > base_len && abs[len - 1] == '/') {
        abs[--len] = '\0';
    }
    return 0;
}

void fs_rel(const char *abs, char *rel, size_t rel_len)
{
    const size_t base_len = strlen(s_base);
    if (strncmp(abs, s_base, base_len) == 0 && (abs[base_len] == '/' || abs[base_len] == '\0')) {
        abs += base_len;
    }
    snprintf(rel, rel_len, "%s", abs[0] ? abs : "/");
}

/* ------------------------------------------------------------------ reading */

int fs_usage(size_t *total, size_t *used)
{
    if (s_info == NULL) {
        return ENOTSUP;
    }
    return s_info(s_info_ctx, total, used) == ESP_OK ? 0 : EIO;
}

static int free_space(size_t *out)
{
    size_t total = 0, used = 0;
    const int err = fs_usage(&total, &used);
    *out = total > used ? total - used : 0;
    return err;
}

static const char *base_name(const char *abs)
{
    const char *slash = strrchr(abs, '/');
    return slash ? slash + 1 : abs;
}

int fs_stat_entry(const char *abs, fs_entry_t *out)
{
    struct stat st;
    if (stat(abs, &st) != 0) {
        return errno;
    }
    strlcpy(out->name, is_root(abs) ? "" : base_name(abs), sizeof(out->name));
    out->dir = S_ISDIR(st.st_mode);
    out->size = out->dir ? 0 : (size_t)st.st_size;
    out->mtime = st.st_mtime > SANE_TIME ? st.st_mtime : 0;
    return 0;
}

int fs_list(const char *abs, fs_list_cb_t cb, void *ctx)
{
    struct stat st;
    if (stat(abs, &st) != 0) {
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return ENOTDIR;
    }
    DIR *dir = opendir(abs);
    if (dir == NULL) {
        return errno;
    }
    char full[FS_ABS_MAX + FS_NAME_MAX];
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        snprintf(full, sizeof(full), "%s/%s", abs, de->d_name);
        fs_entry_t e;
        if (fs_stat_entry(full, &e) != 0) {
            continue;       /* gone since readdir */
        }
        if (!cb(&e, ctx)) {
            break;
        }
    }
    closedir(dir);
    return 0;
}

static void to_hex(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

/* SHA-256 through PSA, which is all mbedTLS 4 offers; the S3's SHA engine does the work. */
static bool sha_begin(psa_hash_operation_t *op)
{
    *op = psa_hash_operation_init();
    return psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(op, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

static bool sha_end(psa_hash_operation_t *op, char hex[65])
{
    uint8_t digest[32];
    size_t len = 0;
    if (psa_hash_finish(op, digest, sizeof(digest), &len) != PSA_SUCCESS || len != sizeof(digest)) {
        psa_hash_abort(op);
        return false;
    }
    to_hex(digest, sizeof(digest), hex);
    return true;
}

int fs_hash(const char *abs, char hex[65])
{
    struct stat st;
    if (stat(abs, &st) != 0) {
        return errno;
    }
    if (S_ISDIR(st.st_mode)) {
        return EISDIR;
    }
    const int fd = open(abs, O_RDONLY);
    if (fd < 0) {
        return errno;
    }
    uint8_t *buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_8BIT);
    psa_hash_operation_t op;
    if (buf == NULL || !sha_begin(&op)) {
        free(buf);
        close(fd);
        return ENOMEM;
    }
    ssize_t n;
    while ((n = read(fd, buf, IO_CHUNK)) > 0) {
        psa_hash_update(&op, buf, (size_t)n);
    }
    const int err = n < 0 ? errno : 0;
    close(fd);
    free(buf);
    if (err != 0) {
        psa_hash_abort(&op);
        return err;
    }
    return sha_end(&op, hex) ? 0 : EIO;
}

/* ------------------------------------------------------------------ changing */

/* Depth first. `path` is a buffer of FS_ABS_MAX + FS_NAME_MAX, extended and cut back in place. */
static int remove_tree(char *path, size_t cap)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : errno;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return errno;
    }
    const size_t len = strlen(path);
    int err = 0;
    struct dirent *de;
    while (err == 0 && (de = readdir(dir)) != NULL) {
        if (snprintf(path + len, cap - len, "/%s", de->d_name) >= (int)(cap - len)) {
            err = ENAMETOOLONG;
            break;
        }
        err = remove_tree(path, cap);
        path[len] = '\0';
    }
    closedir(dir);
    if (err == 0 && rmdir(path) != 0) {
        err = errno;
    }
    return err;
}

int fs_remove(const char *abs, bool recursive)
{
    if (is_root(abs)) {
        return EBUSY;
    }
    struct stat st;
    if (stat(abs, &st) != 0) {
        return errno;
    }
    changing(abs);
    if (!S_ISDIR(st.st_mode)) {
        return unlink(abs) == 0 ? 0 : errno;
    }
    if (!recursive) {
        if (rmdir(abs) == 0) {
            return 0;
        }
        return errno == EEXIST ? ENOTEMPTY : errno;     /* some filesystems say EEXIST */
    }
    char path[FS_ABS_MAX + FS_NAME_MAX];
    strlcpy(path, abs, sizeof(path));
    return remove_tree(path, sizeof(path));
}

int fs_mkdir(const char *abs, bool parents)
{
    struct stat st;
    if (stat(abs, &st) == 0) {
        return EEXIST;
    }
    if (!parents) {
        return mkdir(abs, 0775) == 0 ? 0 : errno;
    }
    /* Each directory on the way, from just under the root */
    char path[FS_ABS_MAX];
    strlcpy(path, abs, sizeof(path));
    for (char *p = path + strlen(s_base) + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            const char c = *p;
            *p = '\0';
            if (stat(path, &st) != 0) {
                if (mkdir(path, 0775) != 0) {
                    return errno;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                return ENOTDIR;
            }
            if (c == '\0') {
                break;
            }
            *p = c;
        }
    }
    return 0;
}

int fs_move(const char *from, const char *to, bool overwrite)
{
    if (is_root(from) || is_root(to)) {
        return EBUSY;
    }
    struct stat sf, st;
    if (stat(from, &sf) != 0) {
        return errno;
    }
    const size_t fl = strlen(from);
    if (strncmp(to, from, fl) == 0 && to[fl] == '/') {
        return EINVAL;                  /* a directory into itself */
    }
    if (strcmp(from, to) == 0) {
        return 0;
    }
    if (stat(to, &st) == 0) {
        if (!overwrite) {
            return EEXIST;
        }
        if (S_ISDIR(st.st_mode) || S_ISDIR(sf.st_mode)) {
            return S_ISDIR(st.st_mode) ? EISDIR : EEXIST;
        }
        changing(to);
        if (unlink(to) != 0) {
            return errno;
        }
    }
    changing(from);
    return rename(from, to) == 0 ? 0 : errno;
}

/* ------------------------------------------------------------------ writing */

struct fs_writer {
    int fd;
    psa_hash_operation_t sha;
    bool overwrite;
    char path[FS_ABS_MAX];
    char part[FS_ABS_MAX + 8];
};

int fs_write_begin(const char *abs, size_t size, bool overwrite, fs_writer_t **out)
{
    *out = NULL;
    if (is_root(abs)) {
        return EISDIR;
    }
    struct stat st;
    if (stat(abs, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return EISDIR;
        }
        if (!overwrite) {
            return EEXIST;
        }
    }
    size_t avail = 0;
    if (size && free_space(&avail) == 0 && size + SPACE_MARGIN > avail) {
        return ENOSPC;      /* a replaced file's space comes back only after the rename */
    }
    fs_writer_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ENOMEM;
    }
    w->overwrite = overwrite;
    strlcpy(w->path, abs, sizeof(w->path));
    snprintf(w->part, sizeof(w->part), "%s.part", abs);
    w->fd = open(w->part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) {
        const int err = errno;
        free(w);
        return err;
    }
    if (!sha_begin(&w->sha)) {
        close(w->fd);
        unlink(w->part);
        free(w);
        return ENOMEM;
    }
    *out = w;
    return 0;
}

int fs_write(fs_writer_t *w, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = write(w->fd, p + off, len - off);
        if (n <= 0) {
            return n < 0 ? errno : ENOSPC;
        }
        off += (size_t)n;
    }
    psa_hash_update(&w->sha, data, len);
    return 0;
}

void fs_write_abort(fs_writer_t *w)
{
    if (w == NULL) {
        return;
    }
    close(w->fd);
    psa_hash_abort(&w->sha);
    unlink(w->part);
    free(w);
}

int fs_write_finish(fs_writer_t *w, const char *expect_sha256, char *hex_out, bool *replaced)
{
    int err = close(w->fd) == 0 ? 0 : errno;     /* the last of the data is written on close */
    char hex[65] = "";
    if (!sha_end(&w->sha, hex) && err == 0) {
        err = EIO;
    }
    if (err == 0 && expect_sha256 != NULL && expect_sha256[0] != '\0' && strcasecmp(hex, expect_sha256) != 0) {
        err = EBADMSG;
    }
    struct stat st;
    const bool exists = stat(w->path, &st) == 0;
    if (err == 0 && exists && !w->overwrite) {
        err = EEXIST;       /* it appeared while this was written */
    }
    if (err == 0 && exists) {
        changing(w->path);
        if (unlink(w->path) != 0) {
            err = errno;
        }
    }
    if (err == 0 && rename(w->part, w->path) != 0) {
        err = errno;
    }
    if (err != 0) {
        unlink(w->part);
    }
    if (hex_out != NULL) {
        strlcpy(hex_out, hex, 65);
    }
    if (replaced != NULL) {
        *replaced = exists;
    }
    free(w);
    return err;
}

int fs_copy(const char *from, const char *to, bool overwrite)
{
    struct stat st;
    if (stat(from, &st) != 0) {
        return errno;
    }
    if (S_ISDIR(st.st_mode)) {
        return EISDIR;
    }
    if (strcmp(from, to) == 0) {
        return EEXIST;
    }
    const int fd = open(from, O_RDONLY);
    if (fd < 0) {
        return errno;
    }
    fs_writer_t *w;
    int err = fs_write_begin(to, (size_t)st.st_size, overwrite, &w);
    uint8_t *buf = err == 0 ? heap_caps_malloc(IO_CHUNK, MALLOC_CAP_8BIT) : NULL;
    if (err == 0 && buf == NULL) {
        err = ENOMEM;
    }
    ssize_t n = 0;
    while (err == 0 && (n = read(fd, buf, IO_CHUNK)) > 0) {
        err = fs_write(w, buf, (size_t)n);
    }
    if (err == 0 && n < 0) {
        err = errno;
    }
    close(fd);
    free(buf);
    if (w == NULL) {
        return err;
    }
    if (err != 0) {
        fs_write_abort(w);
        return err;
    }
    return fs_write_finish(w, NULL, NULL, NULL);
}
