/*
 * fs_ops -- the file operations behind `fs`, for other callers (the HTTP API): list, stat,
 * make, move, copy, delete, hash, and write a file safely. Plain POSIX on the volume
 * register_fs() (or fs_ops_init()) names, so any VFS volume works. See fs_ops.c.
 *
 * Paths here are absolute on the board ("/data/clips/a.mov"); fs_path() makes one from a path
 * relative to the volume ("/clips/a.mov"), checked. Everything returns 0 or an errno value:
 * ENOENT, EEXIST, ENOTEMPTY, EISDIR, ENOTDIR, ENOSPC, EINVAL, EBADMSG (a SHA-256 that did not
 * match), EBUSY (the volume root), or whatever the filesystem said.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_PATH_MAX  160    /* a volume-relative path is shorter: at most 159 bytes */
#define FS_NAME_MAX  63     /* one name in it */
#define FS_ABS_MAX   (FS_PATH_MAX + 64)

typedef esp_err_t (*fs_info_fn_t)(void *ctx, size_t *total_bytes, size_t *used_bytes);

/* The volume: its mount point ("/data") and, optionally, its size. register_fs() calls this. */
void fs_ops_init(const char *base_path, fs_info_fn_t info, void *info_ctx);
const char *fs_root(void);  /* "" before fs_ops_init() */

/*
 * Before anything at `abs` (a file, or a directory and all under it) is replaced, moved or
 * deleted -- by the console or the API -- so what is using it can let go (a clip playing from
 * it). One hook; a later call replaces it.
 */
typedef void (*fs_change_hook_t)(const char *abs, void *ctx);
void fs_ops_set_change_hook(fs_change_hook_t hook, void *ctx);

/*
 * A path relative to the volume -- "/clips/a.mov"; "/" the root; "/data/clips/a.mov" also
 * accepted -- as an absolute one, checked: no "..", ".", empty segments, backslashes or control
 * characters, names of at most FS_NAME_MAX bytes, at most FS_PATH_MAX - 1 in all. EINVAL with
 * `why` otherwise.
 */
int fs_path(const char *rel, char *abs, size_t abs_len, char *why, size_t why_len);

/* An absolute path as relative to the volume: "/data/clips/a.mov" -> "/clips/a.mov". */
void fs_rel(const char *abs, char *rel, size_t rel_len);

int fs_usage(size_t *total, size_t *used);

typedef struct {
    char name[FS_NAME_MAX + 1];
    bool dir;
    size_t size;
    time_t mtime;           /* 0 when the clock was not set when it changed */
} fs_entry_t;

int fs_stat_entry(const char *abs, fs_entry_t *out);

/* Each entry of a directory, in the filesystem's order; `cb` returns false to stop. */
typedef bool (*fs_list_cb_t)(const fs_entry_t *e, void *ctx);
int fs_list(const char *abs, fs_list_cb_t cb, void *ctx);

int fs_remove(const char *abs, bool recursive);    /* a file, or a directory (empty unless recursive) */
int fs_mkdir(const char *abs, bool parents);
int fs_move(const char *from, const char *to, bool overwrite);   /* overwrite: a file, never a directory */
int fs_copy(const char *from, const char *to, bool overwrite);   /* files; through a .part, as a write */
int fs_hash(const char *abs, char hex[65]);

/*
 * Writing a file: into `<path>.part`, hashed as it goes, and renamed over `<path>` only by
 * fs_write_finish() -- so a write that fails or is abandoned leaves nothing half-done, nor
 * costs the file it would replace. `size`, if known, is checked against the free space less a
 * margin LittleFS needs to stay writable (ENOSPC).
 */
typedef struct fs_writer fs_writer_t;
int fs_write_begin(const char *abs, size_t size, bool overwrite, fs_writer_t **out);
int fs_write(fs_writer_t *w, const void *data, size_t len);
/* Close, check `expect_sha256` if given (EBADMSG), and put it in place. `hex_out` (65 bytes,
 * optional) gets its SHA-256. Frees `w`, whatever it returns. */
int fs_write_finish(fs_writer_t *w, const char *expect_sha256, char *hex_out, bool *replaced);
void fs_write_abort(fs_writer_t *w);

#ifdef __cplusplus
}
#endif
