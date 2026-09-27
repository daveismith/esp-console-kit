/*
 * web_fs: the /api/v1/fs routes. See web_fs.h.
 *
 * Paths are a `path` query parameter (or a JSON body's `path`, `from`, `to`) from the root of
 * the volume, checked by fs_path(). Uploads, downloads, copies and hashes are long operations
 * (web_job_start()): one at a time, on a task of their own.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"
#include "web_fs.h"
#include "web_server.h"

#define CHUNK    4096
#define REL_MAX  (FS_PATH_MAX + 8)

/* ------------------------------------------------------------------ helpers */

bool web_fs_resolve(httpd_req_t *req, const char *rel, char *abs, size_t abs_len)
{
    char why[112];
    if (fs_path(rel, abs, abs_len, why, sizeof(why)) != 0) {
        web_send_error(req, 400, "bad_path", "%s", why);
        return false;
    }
    return true;
}

/* The query's `key` as an absolute path; `dflt` when absent, if not NULL. False after replying. */
static bool query_path(httpd_req_t *req, const char *key, const char *dflt, char *abs)
{
    char rel[256];
    if (!web_query(req, key, rel, sizeof(rel)) || rel[0] == '\0') {
        if (dflt == NULL) {
            web_send_error(req, 400, "bad_request", "give `%s`, from the root of the volume: /clips/intro.mov", key);
            return false;
        }
        strlcpy(rel, dflt, sizeof(rel));
    }
    return web_fs_resolve(req, rel, abs, FS_ABS_MAX);
}

/* A JSON body's string `key` as an absolute path. False after replying. */
static bool body_path(httpd_req_t *req, const cJSON *body, const char *key, char *abs)
{
    const cJSON *v = cJSON_GetObjectItem(body, key);
    if (!cJSON_IsString(v)) {
        web_send_error(req, 400, "bad_request", "give `%s`, a path from the root of the volume", key);
        return false;
    }
    return web_fs_resolve(req, v->valuestring, abs, FS_ABS_MAX);
}

static bool body_bool(const cJSON *body, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(body, key));
}

esp_err_t web_fs_send_errno(httpd_req_t *req, int err, const char *abs)
{
    char rel[REL_MAX];
    fs_rel(abs, rel, sizeof(rel));
    switch (err) {
    case ENOENT:    return web_send_error(req, 404, "not_found", "no such file or directory: %s", rel);
    case EEXIST:    return web_send_error(req, 409, "exists", "%s exists (overwrite=true replaces it)", rel);
    case ENOTEMPTY: return web_send_error(req, 409, "not_empty", "%s is not empty (recursive=true deletes it and "
                                          "everything in it)", rel);
    case EISDIR:    return web_send_error(req, 400, "is_a_directory", "%s is a directory", rel);
    case ENOTDIR:   return web_send_error(req, 400, "not_a_directory", "%s is not a directory", rel);
    case ENOSPC:    return web_send_error(req, 507, "no_space", "the volume is too full for %s", rel);
    case EBADMSG:   return web_send_error(req, 422, "sha256_mismatch", "%s did not arrive intact: its SHA-256 "
                                          "differs; nothing was changed", rel);
    case EBUSY:     return web_send_error(req, 400, "bad_path", "not the root of the volume");
    default:        return web_send_error(req, 500, "failed", "%s: %s", rel, strerror(err));
    }
}

cJSON *web_fs_entry_json(const char *abs, const fs_entry_t *e)
{
    char rel[REL_MAX];
    fs_rel(abs, rel, sizeof(rel));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "path", rel);
    cJSON_AddStringToObject(o, "type", e->dir ? "dir" : "file");
    cJSON_AddNumberToObject(o, "size", (double)e->size);
    if (e->mtime != 0) {
        char when[24];
        struct tm tm;
        gmtime_r(&e->mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
        cJSON_AddStringToObject(o, "mtime", when);
    } else {
        cJSON_AddNullToObject(o, "mtime");
    }
    return o;
}

/* The entry at `abs`, as a reply with `status`; or its errno. */
static esp_err_t send_entry(httpd_req_t *req, int status, const char *abs)
{
    fs_entry_t e;
    const int err = fs_stat_entry(abs, &e);
    if (err != 0) {
        return web_fs_send_errno(req, err, abs);
    }
    return web_send_json(req, status, web_fs_entry_json(abs, &e));
}

/* The directory `abs` is in must be there: false after replying (404, or 400 not a directory). */
static bool parent_ok(httpd_req_t *req, const char *abs)
{
    char parent[FS_ABS_MAX];
    strlcpy(parent, abs, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        return true;
    }
    *slash = '\0';
    struct stat st;
    char rel[REL_MAX];
    fs_rel(parent, rel, sizeof(rel));
    if (stat(parent, &st) != 0) {
        web_send_error(req, 404, "not_found", "no directory %s (parents=true makes it)", rel);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        web_send_error(req, 400, "not_a_directory", "%s is not a directory", rel);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ GET /fs */

static esp_err_t volume_get(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    fs_usage(&total, &used);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "root", fs_root());
    cJSON_AddNumberToObject(o, "total", (double)total);
    cJSON_AddNumberToObject(o, "used", (double)used);
    cJSON_AddNumberToObject(o, "free", (double)(total > used ? total - used : 0));
    return web_send_json(req, 200, o);
}

/* ------------------------------------------------------------------ GET /fs/list */

typedef struct {
    fs_entry_t *items;
    size_t n, cap;
} listing_t;

static bool collect(const fs_entry_t *e, void *ctx)
{
    listing_t *l = ctx;
    if (l->n == l->cap) {
        const size_t cap = l->cap ? 2 * l->cap : 16;
        fs_entry_t *items = realloc(l->items, cap * sizeof(*items));
        if (items == NULL) {
            return false;
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->n++] = *e;
    return true;
}

static int by_kind_then_name(const void *a, const void *b)
{
    const fs_entry_t *x = a, *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

static esp_err_t list_get(httpd_req_t *req)
{
    char abs[FS_ABS_MAX];
    if (!query_path(req, "path", "/", abs)) {
        return ESP_OK;
    }
    listing_t l = { 0 };
    const int err = fs_list(abs, collect, &l);
    if (err != 0) {
        free(l.items);
        return web_fs_send_errno(req, err, abs);
    }
    qsort(l.items, l.n, sizeof(*l.items), by_kind_then_name);
    char rel[REL_MAX];
    fs_rel(abs, rel, sizeof(rel));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", rel);
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    char child[FS_ABS_MAX + FS_NAME_MAX + 1];
    for (size_t i = 0; i < l.n; i++) {
        snprintf(child, sizeof(child), "%s/%s", abs, l.items[i].name);
        cJSON_AddItemToArray(entries, web_fs_entry_json(child, &l.items[i]));
    }
    free(l.items);
    return web_send_json(req, 200, root);
}

/* ------------------------------------------------------------------ /fs/entry */

static void hash_job(httpd_req_t *req, void *ctx)
{
    char *abs = ctx;
    fs_entry_t e;
    char hex[65];
    int err = fs_stat_entry(abs, &e);
    if (err == 0) {
        err = fs_hash(abs, hex);
    }
    if (err != 0) {
        web_fs_send_errno(req, err, abs);
    } else {
        cJSON *o = web_fs_entry_json(abs, &e);
        cJSON_AddStringToObject(o, "sha256", hex);
        web_send_json(req, 200, o);
    }
    free(abs);
}

static esp_err_t entry_get(httpd_req_t *req)
{
    char abs[FS_ABS_MAX];
    if (!query_path(req, "path", NULL, abs)) {
        return ESP_OK;
    }
    fs_entry_t e;
    const int err = fs_stat_entry(abs, &e);
    if (err != 0) {
        return web_fs_send_errno(req, err, abs);
    }
    if (!web_query_bool(req, "sha256", false) || e.dir) {
        return web_send_json(req, 200, web_fs_entry_json(abs, &e));
    }
    char *ctx = strdup(abs);
    if (ctx == NULL) {
        return httpd_resp_send_500(req);
    }
    if (web_job_start(req, "a SHA-256", hash_job, ctx) != ESP_OK) {
        free(ctx);
    }
    return ESP_OK;
}

static esp_err_t entry_delete(httpd_req_t *req)
{
    char abs[FS_ABS_MAX];
    if (!query_path(req, "path", NULL, abs)) {
        return ESP_OK;
    }
    if (strcmp(abs, fs_root()) == 0) {
        return web_send_error(req, 400, "bad_path", "the root of the volume cannot be deleted");
    }
    const int err = fs_remove(abs, web_query_bool(req, "recursive", false));
    if (err != 0) {
        return web_fs_send_errno(req, err, abs);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ GET /fs/file */

static const char *type_for(const char *name)
{
    static const struct { const char *ext, *type; } TYPES[] = {
        { ".mov", "video/quicktime" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".txt", "text/plain; charset=utf-8" },
        { ".log", "text/plain; charset=utf-8" }, { ".json", "application/json" },
    };
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot != NULL && i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcasecmp(dot, TYPES[i].ext) == 0) {
            return TYPES[i].type;
        }
    }
    return "application/octet-stream";     /* never text/html: it would run as the board's page */
}

static void download_job(httpd_req_t *req, void *ctx)
{
    char *abs = ctx;
    const int fd = open(abs, O_RDONLY);
    char *buf = fd >= 0 ? malloc(CHUNK) : NULL;
    if (fd < 0 || buf == NULL) {
        web_fs_send_errno(req, fd < 0 ? errno : ENOMEM, abs);
    } else {
        const char *name = strrchr(abs, '/') + 1;
        char disposition[FS_NAME_MAX + 40];
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
        httpd_resp_set_type(req, type_for(name));
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        ssize_t n;
        while ((n = read(fd, buf, CHUNK)) > 0) {
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                break;      /* the client has gone */
            }
        }
        httpd_resp_send_chunk(req, NULL, 0);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    free(abs);
}

static esp_err_t file_get(httpd_req_t *req)
{
    char abs[FS_ABS_MAX];
    if (!query_path(req, "path", NULL, abs)) {
        return ESP_OK;
    }
    fs_entry_t e;
    const int err = fs_stat_entry(abs, &e);
    if (err != 0 || e.dir) {
        return web_fs_send_errno(req, err != 0 ? err : EISDIR, abs);
    }
    char *ctx = strdup(abs);
    if (ctx == NULL) {
        return httpd_resp_send_500(req);
    }
    if (web_job_start(req, "a file download", download_job, ctx) != ESP_OK) {
        free(ctx);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ PUT /fs/file */

typedef struct {
    char abs[FS_ABS_MAX];
    char sha256[65];
    bool overwrite;
    bool parents;
} upload_t;

static void upload_job(httpd_req_t *req, void *ctx)
{
    upload_t *u = ctx;
    fs_writer_t *w = NULL;
    int err = 0;
    if (u->parents) {
        char parent[FS_ABS_MAX];
        strlcpy(parent, u->abs, sizeof(parent));
        *strrchr(parent, '/') = '\0';
        struct stat st;
        if (stat(parent, &st) != 0) {
            err = fs_mkdir(parent, true);
        }
    }
    if (err == 0) {
        err = fs_write_begin(u->abs, req->content_len, u->overwrite, &w);
    }
    char *buf = err == 0 ? malloc(CHUNK) : NULL;
    if (err == 0 && buf == NULL) {
        err = ENOMEM;
    }
    size_t left = req->content_len;
    int timeouts = 0;
    bool gone = false;
    while (err == 0 && left > 0) {
        const int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            gone = true;    /* cut off: nothing to answer, and nothing written */
            break;
        }
        timeouts = 0;
        err = fs_write(w, buf, n);
        left -= n;
    }
    free(buf);
    bool replaced = false;
    if (w != NULL) {
        if (err != 0 || gone) {
            fs_write_abort(w);
        } else {
            err = fs_write_finish(w, u->sha256, NULL, &replaced);
        }
    }
    if (!gone) {
        if (err != 0) {
            web_fs_send_errno(req, err, u->abs);
        } else {
            send_entry(req, replaced ? 200 : 201, u->abs);
        }
    }
    free(u);
}

static esp_err_t file_put(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "Content-Length") == 0) {
        return web_send_error(req, 411, "length_required", "send the file as the body, with a Content-Length");
    }
    upload_t *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        return httpd_resp_send_500(req);
    }
    if (!query_path(req, "path", NULL, u->abs)) {
        free(u);
        return ESP_OK;
    }
    web_query(req, "sha256", u->sha256, sizeof(u->sha256));
    if (u->sha256[0] != '\0' && (strlen(u->sha256) != 64 || strspn(u->sha256, "0123456789abcdefABCDEF") != 64)) {
        free(u);
        return web_send_error(req, 400, "bad_sha256", "sha256 is 64 hex digits");
    }
    u->overwrite = web_query_bool(req, "overwrite", false);
    u->parents = web_query_bool(req, "parents", false);

    /* What can be refused before a byte is read, is */
    struct stat st;
    int err = strcmp(u->abs, fs_root()) == 0 ? EISDIR : 0;
    if (err == 0 && stat(u->abs, &st) == 0) {
        err = S_ISDIR(st.st_mode) ? EISDIR : u->overwrite ? 0 : EEXIST;
    }
    if (err != 0) {
        web_fs_send_errno(req, err, u->abs);
        free(u);
        return ESP_OK;
    }
    if (!u->parents && !parent_ok(req, u->abs)) {
        free(u);
        return ESP_OK;
    }
    size_t total = 0, used = 0;
    if (fs_usage(&total, &used) == 0 && req->content_len + 32 * 1024 > (total > used ? total - used : 0)) {
        web_send_error(req, 507, "no_space", "%u bytes will not fit: %u free, less a 32768-byte margin",
                       (unsigned)req->content_len, (unsigned)(total > used ? total - used : 0));
        free(u);
        return ESP_OK;
    }
    if (web_job_start(req, "a file upload", upload_job, u) != ESP_OK) {
        free(u);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ POST /fs/mkdir, move, copy */

static esp_err_t mkdir_post(httpd_req_t *req)
{
    cJSON *body = web_read_json(req, 512);
    if (body == NULL) {
        return ESP_OK;
    }
    char abs[FS_ABS_MAX];
    const bool ok = body_path(req, body, "path", abs);
    const bool parents = body_bool(body, "parents");
    cJSON_Delete(body);
    if (!ok) {
        return ESP_OK;
    }
    if (!parents && !parent_ok(req, abs)) {
        return ESP_OK;
    }
    const int err = fs_mkdir(abs, parents);
    return err != 0 ? web_fs_send_errno(req, err, abs) : send_entry(req, 201, abs);
}

/* `from` and `to` from the body, both checked, `to`'s directory there. False after replying. */
static bool from_to(httpd_req_t *req, char *from, char *to, bool *overwrite)
{
    cJSON *body = web_read_json(req, 768);
    if (body == NULL) {
        return false;
    }
    const bool ok = body_path(req, body, "from", from) && body_path(req, body, "to", to);
    *overwrite = body_bool(body, "overwrite");
    cJSON_Delete(body);
    if (!ok) {
        return false;
    }
    struct stat st;
    if (stat(from, &st) != 0) {
        web_fs_send_errno(req, errno, from);
        return false;
    }
    return parent_ok(req, to);
}

static esp_err_t move_post(httpd_req_t *req)
{
    char from[FS_ABS_MAX], to[FS_ABS_MAX];
    bool overwrite;
    if (!from_to(req, from, to, &overwrite)) {
        return ESP_OK;
    }
    const int err = fs_move(from, to, overwrite);
    if (err == EINVAL) {
        return web_send_error(req, 400, "bad_request", "a directory cannot move into itself");
    }
    if (err == EISDIR) {
        char rel[REL_MAX];
        fs_rel(to, rel, sizeof(rel));
        return web_send_error(req, 409, "exists", "%s is a directory, which is never replaced", rel);
    }
    return err != 0 ? web_fs_send_errno(req, err, err == EEXIST ? to : from) : send_entry(req, 200, to);
}

typedef struct {
    char from[FS_ABS_MAX];
    char to[FS_ABS_MAX];
    bool overwrite;
} copy_t;

static void copy_job(httpd_req_t *req, void *ctx)
{
    copy_t *c = ctx;
    const int err = fs_copy(c->from, c->to, c->overwrite);
    if (err != 0) {
        web_fs_send_errno(req, err, err == EEXIST || err == ENOSPC ? c->to : c->from);
    } else {
        send_entry(req, 201, c->to);
    }
    free(c);
}

static esp_err_t copy_post(httpd_req_t *req)
{
    copy_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return httpd_resp_send_500(req);
    }
    if (!from_to(req, c->from, c->to, &c->overwrite)) {
        free(c);
        return ESP_OK;
    }
    struct stat st;
    int err = 0;
    if (stat(c->from, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(c);
        return web_send_error(req, 400, "is_a_directory", "only files are copied");
    }
    if (stat(c->to, &st) == 0) {
        err = S_ISDIR(st.st_mode) ? EISDIR : c->overwrite ? 0 : EEXIST;
    }
    if (err != 0) {
        web_fs_send_errno(req, err, c->to);
        free(c);
        return ESP_OK;
    }
    if (web_job_start(req, "a copy", copy_job, c) != ESP_OK) {
        free(c);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t web_fs_register(void)
{
    web_server_add_feature("files");
    esp_err_t err = ESP_OK;
    err |= web_register("/api/v1/fs", HTTP_GET, volume_get, 0);
    err |= web_register("/api/v1/fs/list", HTTP_GET, list_get, 0);
    err |= web_register("/api/v1/fs/entry", HTTP_GET, entry_get, 0);
    err |= web_register("/api/v1/fs/entry", HTTP_DELETE, entry_delete, WEB_AUTH);
    err |= web_register("/api/v1/fs/file", HTTP_GET, file_get, WEB_AUTH);
    err |= web_register("/api/v1/fs/file", HTTP_PUT, file_put, WEB_AUTH);
    err |= web_register("/api/v1/fs/mkdir", HTTP_POST, mkdir_post, WEB_AUTH);
    err |= web_register("/api/v1/fs/move", HTTP_POST, move_post, WEB_AUTH);
    err |= web_register("/api/v1/fs/copy", HTTP_POST, copy_post, WEB_AUTH);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}
