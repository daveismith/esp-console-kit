/*
 * `fs`: manage a mounted filesystem from the console, and move files over the console UART
 * with XMODEM-1K. Everything but `df` is plain POSIX, so any VFS volume works (LittleFS,
 * FAT, ...). The host side is tools/fs_xfer.py.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "cmd_fs.h"
#include "xfer_session.h"
#include "xmodem.h"

#define PATH_LEN         160
#define IO_CHUNK         (16 * 1024)
#define BENCH_DEFAULT_KB 512
#define HEXDUMP_DEFAULT  256
#define HEXDUMP_MAX      4096
/* Left free after an upload: LittleFS needs spare blocks to stay writable. */
#define SPACE_MARGIN     (32 * 1024)

static cmd_fs_config_t s_cfg;
static char s_base[64];

/* ------------------------------------------------------------------ helpers */

/* "clip.mov", "/clip.mov" and "/data/clip.mov" all name the same file on a volume at /data. */
static bool resolve(const char *in, char *out, size_t out_len)
{
    const size_t base_len = strlen(s_base);
    int n;
    if (in == NULL || in[0] == '\0' || strcmp(in, "/") == 0) {
        n = snprintf(out, out_len, "%s", s_base);
    } else if (strncmp(in, s_base, base_len) == 0 && (in[base_len] == '/' || in[base_len] == '\0')) {
        n = snprintf(out, out_len, "%s", in);
    } else if (in[0] == '/') {
        n = snprintf(out, out_len, "%s%s", s_base, in);
    } else {
        n = snprintf(out, out_len, "%s/%s", s_base, in);
    }
    if (n < 0 || (size_t)n >= out_len) {
        printf("fs: path too long: %s\n", in);
        return false;
    }
    size_t len = strlen(out);
    while (len > base_len && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return true;
}

static int fail_errno(const char *what, const char *path)
{
    printf("fs: %s %s: %s\n", what, path, strerror(errno));
    return 1;
}

static double seconds_since(int64_t t0_us)
{
    return (double)(esp_timer_get_time() - t0_us) / 1e6;
}

static double kib_per_s(size_t bytes, double secs)
{
    return secs > 0 ? (double)bytes / 1024.0 / secs : 0.0;
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

static bool free_space(size_t *out)
{
    size_t total = 0, used = 0;
    if (s_cfg.info == NULL || s_cfg.info(s_cfg.info_ctx, &total, &used) != ESP_OK) {
        return false;
    }
    *out = total > used ? total - used : 0;
    return true;
}

/* ------------------------------------------------------------------ plain commands */

static int fs_ls(const char *arg)
{
    char path[PATH_LEN];
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return fail_errno("ls", path);
    }
    if (!S_ISDIR(st.st_mode)) {
        printf("%10ld  %s\n", (long)st.st_size, path);
        return 0;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return fail_errno("ls", path);
    }
    unsigned files = 0, dirs = 0;
    uint64_t total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char full[PATH_LEN + 1 + sizeof(de->d_name)];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        const bool ok = stat(full, &st) == 0;
        if (ok && S_ISDIR(st.st_mode)) {
            printf("%10s  %s/\n", "<dir>", de->d_name);
            dirs++;
        } else {
            printf("%10ld  %s\n", ok ? (long)st.st_size : -1L, de->d_name);
            total += ok ? (uint64_t)st.st_size : 0;
            files++;
        }
    }
    closedir(dir);
    printf("%u file%s, %" PRIu64 " bytes", files, files == 1 ? "" : "s", total);
    if (dirs) {
        printf("; %u director%s", dirs, dirs == 1 ? "y" : "ies");
    }
    printf("\n");
    return 0;
}

static int fs_df(void)
{
    size_t total = 0, used = 0;
    if (s_cfg.info == NULL) {
        printf("fs: no usage information for this volume\n");
        return 1;
    }
    esp_err_t err = s_cfg.info(s_cfg.info_ctx, &total, &used);
    if (err != ESP_OK) {
        printf("fs: df: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("%s: %u KB used of %u KB, %u KB free\n", s_base, (unsigned)(used / 1024),
           (unsigned)(total / 1024), (unsigned)((total - used) / 1024));
    return 0;
}

static int fs_stat(const char *arg)
{
    char path[PATH_LEN];
    struct stat st;
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    if (stat(path, &st) != 0) {
        return fail_errno("stat", path);
    }
    printf("%s: %s, %ld bytes", path, S_ISDIR(st.st_mode) ? "directory" : "file", (long)st.st_size);
    /* Only a clock that has been set gives a modification time worth printing. */
    if (st.st_mtime > 1577836800) {   /* 2020-01-01 */
        char when[32];
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        printf(", modified %s", when);
    }
    printf("\n");
    return 0;
}

static int fs_simple(const char *op, const char *arg)
{
    char path[PATH_LEN];
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    if (strcmp(path, s_base) == 0) {
        printf("fs: %s: not the volume root\n", op);
        return 1;
    }
    int rc;
    if (strcmp(op, "mkdir") == 0) {
        rc = mkdir(path, 0775);
    } else if (strcmp(op, "rmdir") == 0) {
        rc = rmdir(path);
    } else {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("fs: rm %s: is a directory (use rmdir)\n", path);
            return 1;
        }
        rc = unlink(path);
    }
    return rc == 0 ? 0 : fail_errno(op, path);
}

static int fs_mv(const char *from_arg, const char *to_arg)
{
    char from[PATH_LEN], to[PATH_LEN];
    if (!resolve(from_arg, from, sizeof(from)) || !resolve(to_arg, to, sizeof(to))) {
        return 1;
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        printf("fs: mv: %s exists\n", to);
        return 1;
    }
    return rename(from, to) == 0 ? 0 : fail_errno("mv", from);
}

static int fs_cat(const char *arg)
{
    char path[PATH_LEN];
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return fail_errno("cat", path);
    }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
    printf("\n");
    return 0;
}

static int fs_hexdump(const char *arg, const char *offset_arg, const char *len_arg)
{
    char path[PATH_LEN];
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    const long offset = offset_arg ? strtol(offset_arg, NULL, 0) : 0;
    long len = len_arg ? strtol(len_arg, NULL, 0) : HEXDUMP_DEFAULT;
    if (offset < 0 || len <= 0 || len > HEXDUMP_MAX) {
        printf("fs: hexdump: offset >= 0, length 1..%d\n", HEXDUMP_MAX);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return fail_errno("hexdump", path);
    }
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd);
        return fail_errno("hexdump", path);
    }
    uint8_t row[16];
    long pos = offset;
    while (len > 0) {
        const ssize_t n = read(fd, row, len < 16 ? (size_t)len : sizeof(row));
        if (n <= 0) {
            break;
        }
        printf("%08lx ", pos);
        for (ssize_t i = 0; i < 16; i++) {
            if (i < n) {
                printf(" %02x", row[i]);
            } else {
                printf("   ");
            }
        }
        printf("  |");
        for (ssize_t i = 0; i < n; i++) {
            putchar(row[i] >= 0x20 && row[i] < 0x7f ? row[i] : '.');
        }
        printf("|\n");
        pos += n;
        len -= n;
    }
    close(fd);
    return 0;
}

static int fs_sha256(const char *arg)
{
    char path[PATH_LEN];
    if (!resolve(arg, path, sizeof(path))) {
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return fail_errno("sha256", path);
    }
    uint8_t *buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_8BIT);
    psa_hash_operation_t op;
    if (buf == NULL || !sha_begin(&op)) {
        printf("fs: sha256: out of memory or no SHA engine\n");
        free(buf);
        close(fd);
        return 1;
    }
    const int64_t t0 = esp_timer_get_time();
    size_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf, IO_CHUNK)) > 0) {
        psa_hash_update(&op, buf, (size_t)n);
        total += (size_t)n;
    }
    const int read_errno = errno;
    close(fd);
    free(buf);
    char hex[65];
    if (n < 0 || !sha_end(&op, hex)) {
        psa_hash_abort(&op);
        printf("fs: sha256 %s: %s\n", path, n < 0 ? strerror(read_errno) : "hash failed");
        return 1;
    }
    const double secs = seconds_since(t0);
    /* sha256sum's own format, so the line compares directly with the host's. */
    printf("%s  %s\n", hex, path);
    printf("%u bytes in %.2f s (%.0f KB/s)\n", (unsigned)total, secs, kib_per_s(total, secs));
    return 0;
}

/* Write a file of `kb` KB in 16 KB chunks, read it back the same way, and delete it. The
 * write figure includes erasing; the read figure is what streaming a file would see. */
static int fs_bench(int kb)
{
    if (kb <= 0) {
        kb = BENCH_DEFAULT_KB;
    }
    const size_t total = (size_t)kb * 1024;
    uint8_t *buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        printf("fs: bench: no memory for a %d byte buffer\n", IO_CHUNK);
        return 1;
    }
    for (size_t i = 0; i < IO_CHUNK; i++) {
        buf[i] = (uint8_t)(i * 31 + 7);
    }
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/.bench", s_base);
    printf("writing %d KB to %s...\n", kb, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return fail_errno("bench", path);
    }
    int64_t t0 = esp_timer_get_time();
    size_t done = 0;
    while (done < total) {
        const size_t n = total - done < IO_CHUNK ? total - done : IO_CHUNK;
        if (write(fd, buf, n) != (ssize_t)n) {
            printf("fs: bench: write failed at %u bytes (volume full?)\n", (unsigned)done);
            close(fd);
            unlink(path);
            free(buf);
            return 1;
        }
        done += n;
    }
    fsync(fd);
    close(fd);
    const double write_s = seconds_since(t0);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        unlink(path);
        free(buf);
        return fail_errno("bench", path);
    }
    t0 = esp_timer_get_time();
    done = 0;
    ssize_t r;
    while ((r = read(fd, buf, IO_CHUNK)) > 0) {
        done += (size_t)r;
    }
    const double read_s = seconds_since(t0);
    close(fd);
    /* Checked after the timing, so it costs the figure nothing. */
    bool ok = true;
    for (size_t i = 0; i < 64 && ok; i++) {
        ok = buf[i] == (uint8_t)(i * 31 + 7);
    }
    unlink(path);
    free(buf);
    printf("write: %u KB in %.2f s, %.0f KB/s (includes erase)\n", (unsigned)(total / 1024),
           write_s, kib_per_s(total, write_s));
    printf("read:  %u KB in %.2f s, %.0f KB/s%s\n", (unsigned)(done / 1024), read_s,
           kib_per_s(done, read_s),
           done != total ? "  *** SHORT READ ***" : ok ? "" : "  *** DATA MISMATCH ***");
    return done == total && ok ? 0 : 1;
}

/* ------------------------------------------------------------------ XMODEM over the UART */

/* The session -- the UART taken raw, logging muted, the rate switched -- is xfer_session.c,
 * shared with `ota put`. */

typedef struct {
    int fd;
    psa_hash_operation_t sha;
} put_ctx_t;

static esp_err_t put_sink(void *ctx, const uint8_t *data, size_t len)
{
    put_ctx_t *p = ctx;
    size_t off = 0;
    while (off < len) {
        const ssize_t w = write(p->fd, data + off, len - off);
        if (w <= 0) {
            return ESP_FAIL;
        }
        off += (size_t)w;
    }
    psa_hash_update(&p->sha, data, len);
    return ESP_OK;
}

static int get_source(void *ctx, uint8_t *buf, size_t len)
{
    const ssize_t n = read(*(int *)ctx, buf, len);
    return n < 0 ? -1 : (int)n;
}

/* `-f` and `-b <baud>` anywhere; the rest positional. Returns the positional count or -1. */
static int parse_transfer_args(int argc, char **argv, bool *force, uint32_t *baud,
                               uint32_t *block, const char **pos, int max_pos)
{
    int count = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && force != NULL) {
            *force = true;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            *baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-s") == 0 && block != NULL && i + 1 < argc) {
            *block = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (*block != 128 && *block != 1024) {
                return -1;
            }
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            return -1;
        } else if (count < max_pos) {
            pos[count++] = argv[i];
        } else {
            return -1;
        }
    }
    return count;
}

/*
 * fs put [-f] [-b baud] <path> [size]
 *
 * Received into <path>.part and renamed over <path> only once complete, so a failed or
 * cancelled upload never leaves a truncated file behind, nor costs the one it was replacing.
 */
static int fs_put(int argc, char **argv)
{
    bool force = false;
    uint32_t baud = 0;
    const char *pos[2] = { 0 };
    const int npos = parse_transfer_args(argc, argv, &force, &baud, NULL, pos, 2);
    if (npos < 1) {
        printf("usage: fs put [-f] [-b baud] <path> [size]\n");
        return 1;
    }
    const size_t size = npos > 1 ? (size_t)strtoul(pos[1], NULL, 10) : 0;

    char path[PATH_LEN], part[PATH_LEN + 8];
    if (!resolve(pos[0], path, sizeof(path))) {
        return 1;
    }
    if (strcmp(path, s_base) == 0) {
        printf("fs: put: give a file name\n");
        return 1;
    }
    struct stat st;
    const bool exists = stat(path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        printf("fs: put: %s is a directory\n", path);
        return 1;
    }
    if (exists && !force) {
        printf("fs: put: %s exists (-f to replace it)\n", path);
        return 1;
    }
    size_t avail = 0;
    if (size && free_space(&avail)) {
        /* A replaced file's space comes back only after the rename. */
        if (size + SPACE_MARGIN > avail) {
            printf("fs: put: %u bytes will not fit in %u free\n", (unsigned)size, (unsigned)avail);
            return 1;
        }
    }
    snprintf(part, sizeof(part), "%s.part", path);

    put_ctx_t ctx = { .fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644) };
    if (ctx.fd < 0) {
        return fail_errno("put", part);
    }
    if (!sha_begin(&ctx.sha)) {
        printf("fs: put: no SHA engine\n");
        close(ctx.fd);
        unlink(part);
        return 1;
    }

    xfer_session_t session;
    const uint32_t console_baud = xfer_console_baud(s_cfg.uart_num);
    char size_text[16];
    snprintf(size_text, sizeof(size_text), size ? "%u" : "?", (unsigned)size);
    printf("xmodem: ready to receive %s bytes at %" PRIu32 " baud: %s\n", size_text,
           baud ? baud : console_baud, path);

    esp_err_t err = xfer_session_begin(&session, s_cfg.uart_num, baud);
    xmodem_stats_t stats = { 0 };
    const int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) {
        const xmodem_config_t xcfg = { .expected_size = size };
        err = xmodem_receive(&session.io, &xcfg, put_sink, &ctx, &stats);
        xfer_session_end(&session);
    }
    const double secs = seconds_since(t0);

    if (close(ctx.fd) != 0 && err == ESP_OK) {
        err = ESP_FAIL;   /* the last of the data is written on close */
    }
    char hex[65] = "";
    const bool hashed = sha_end(&ctx.sha, hex);
    if (err == ESP_OK) {
        if (exists) {
            unlink(path);
        }
        if (rename(part, path) != 0) {
            fail_errno("put: rename", part);
            unlink(part);
            return 1;
        }
    } else {
        unlink(part);
        printf("fs: put failed: %s, after %u bytes (%u retries: %u timed out, %u bad)\n",
               xfer_err(err), (unsigned)stats.bytes, stats.retries, stats.timeouts,
               stats.bad_blocks);
        if (stats.timeouts + stats.bad_blocks) {
            printf("fs: first bad block: %u bytes after the header, starting",
                   (unsigned)stats.diag_len);
            for (size_t i = 0; i < sizeof(stats.diag_head) && i < stats.diag_len; i++) {
                printf(" %02x", stats.diag_head[i]);
            }
            printf("; crc %04x, computed %04x\n", stats.diag_crc_rx, stats.diag_crc_calc);
        }
        return 1;
    }
    printf("received %u bytes in %.1f s (%.1f KB/s, %u retries)\n", (unsigned)stats.bytes, secs,
           kib_per_s(stats.bytes, secs), stats.retries);
    if (hashed) {
        printf("sha256 %s  %s\n", hex, path);
    }
    return 0;
}

/* fs get [-b baud] [-s 128|1024] <path> */
static int fs_get(int argc, char **argv)
{
    uint32_t baud = 0;
    uint32_t block = 1024;
    const char *pos[1] = { 0 };
    if (parse_transfer_args(argc, argv, NULL, &baud, &block, pos, 1) != 1) {
        printf("usage: fs get [-b baud] [-s 128|1024] <path>\n");
        return 1;
    }
    char path[PATH_LEN];
    if (!resolve(pos[0], path, sizeof(path))) {
        return 1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return fail_errno("get", path);
    }
    if (S_ISDIR(st.st_mode)) {
        printf("fs: get: %s is a directory\n", path);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return fail_errno("get", path);
    }

    const uint32_t console_baud = xfer_console_baud(s_cfg.uart_num);
    printf("xmodem: ready to send %ld bytes at %" PRIu32 " baud: %s\n", (long)st.st_size,
           baud ? baud : console_baud, path);

    xfer_session_t session;
    esp_err_t err = xfer_session_begin(&session, s_cfg.uart_num, baud);
    xmodem_stats_t stats = { 0 };
    const int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) {
        const xmodem_config_t xcfg = { .block_size = block };
        err = xmodem_send(&session.io, &xcfg, get_source, &fd, &stats);
        xfer_session_end(&session);
    }
    const double secs = seconds_since(t0);
    close(fd);
    if (err != ESP_OK) {
        printf("fs: get failed: %s, after %u bytes (%u retries)\n", xfer_err(err),
               (unsigned)stats.bytes, stats.retries);
        return 1;
    }
    printf("sent %u bytes in %.1f s (%.1f KB/s, %u retries)\n", (unsigned)stats.bytes, secs,
           kib_per_s(stats.bytes, secs), stats.retries);
    return 0;
}

/* ------------------------------------------------------------------ dispatch */

#define FS_USAGE \
    "usage: fs ls [path] | df | stat <path> | mkdir <path> | rmdir <path> | rm <path>\n" \
    "          | mv <from> <to> | cat <path> | hexdump <path> [offset] [len] | sha256 <path>\n" \
    "          | put [-f] [-b baud] <path> [size] | get [-b baud] [-s 128|1024] <path>\n" \
    "          | bench [kb]\n"

static int fs_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf(FS_USAGE);
        return 1;
    }
    const char *sub = argv[1];
    const char *a1 = argc > 2 ? argv[2] : NULL;
    const char *a2 = argc > 3 ? argv[3] : NULL;

    if (strcmp(sub, "ls") == 0) {
        return fs_ls(a1);
    } else if (strcmp(sub, "df") == 0) {
        return fs_df();
    } else if (strcmp(sub, "bench") == 0) {
        return fs_bench(a1 ? atoi(a1) : 0);
    } else if (strcmp(sub, "put") == 0) {
        return fs_put(argc - 2, argv + 2);
    } else if (strcmp(sub, "get") == 0) {
        return fs_get(argc - 2, argv + 2);
    } else if (a1 == NULL) {
        /* everything below needs a path */
    } else if (strcmp(sub, "stat") == 0) {
        return fs_stat(a1);
    } else if (strcmp(sub, "mkdir") == 0 || strcmp(sub, "rmdir") == 0 || strcmp(sub, "rm") == 0) {
        return fs_simple(sub, a1);
    } else if (strcmp(sub, "mv") == 0 && a2 != NULL) {
        return fs_mv(a1, a2);
    } else if (strcmp(sub, "cat") == 0) {
        return fs_cat(a1);
    } else if (strcmp(sub, "hexdump") == 0) {
        return fs_hexdump(a1, a2, argc > 4 ? argv[4] : NULL);
    } else if (strcmp(sub, "sha256") == 0) {
        return fs_sha256(a1);
    }
    printf(FS_USAGE);
    return 1;
}

esp_err_t register_fs(const cmd_fs_config_t *config)
{
    if (config == NULL || config->base_path == NULL ||
        strlen(config->base_path) >= sizeof(s_base)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *config;
    strlcpy(s_base, config->base_path, sizeof(s_base));
    s_cfg.base_path = s_base;

    const esp_console_cmd_t cmd = {
        .command = "fs",
        .help = "Files on the storage volume: list, inspect, hash, and move them over the console "
                "with XMODEM-1K (host side: esp-console-kit/tools/fs_xfer.py)",
        .hint = "ls|df|stat|mkdir|rmdir|rm|mv|cat|hexdump|sha256|put|get|bench ...",
        .func = fs_cmd,
    };
    return esp_console_cmd_register(&cmd);
}
