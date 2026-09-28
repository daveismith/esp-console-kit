/*
 * GET /api/v1/events: the board's events (components `events`), as Server-Sent Events, or the
 * kept happenings as JSON.
 *
 * A stream is a connection the handler answers with its own head -- text/event-stream, no
 * length -- and then keeps: the server goes on to other requests, and events are written to
 * the socket later, from the server's own task (httpd_queue_work), so nothing here races it. A
 * write that would block, or fails, closes the stream: the browser's EventSource comes back by
 * itself, and a Last-Event-ID replays what it missed. Every 15 s a stream gets `system`, or a
 * comment, so a client that has gone away is found out.
 *
 * Everything that touches the streams runs on the server's task: the handler, the flush, the
 * heartbeat's send, and web_events_closed() (httpd's close_fn).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "events.h"
#include "wifi_known.h"
#include "web_internal.h"
#include "web_server.h"

static const char *TAG = "web_events";

#define HEARTBEAT_US (15 * 1000 * 1000)

typedef struct {
    int fd;                 /* -1: free */
    events_mask_t want;
    uint16_t seq;           /* the last happening it was sent */
} stream_t;

static stream_t s_streams[CONFIG_WEB_SERVER_MAX_STREAMS];
static int s_sink = -1;
static bool s_queued;       /* a flush is on the server's queue */
static esp_timer_handle_t s_heartbeat;
static uint32_t s_sent[EVENTS_MAX_KINDS];  /* a hash of each state as last sent: the same again isn't */

/* FNV-1a */
static uint32_t hash_of(const char *text)
{
    uint32_t h = 2166136261u;
    for (; *text; text++) {
        h = (h ^ (uint8_t)*text) * 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ `system` */

static cJSON *system_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
    wifi_ap_record_t ap;
    if (wifi_known_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(o, "rssi");
    }
    return o;
}

/* ------------------------------------------------------------------ sending */

static void drop(stream_t *s)
{
    if (s->fd >= 0) {
        httpd_handle_t hd = web_server_handle();
        if (hd != NULL) {
            httpd_sess_trigger_close(hd, s->fd);
        }
        s->fd = -1;
    }
}

/* All of it without waiting, or the stream is closed: a client that can't take a few KB is gone. */
static bool send_all(stream_t *s, const char *buf, size_t len)
{
    httpd_handle_t hd = web_server_handle();
    if (s->fd < 0 || hd == NULL) {
        return false;
    }
    const int sent = httpd_socket_send(hd, s->fd, buf, len, MSG_DONTWAIT);
    if (sent != (int)len) {
        ESP_LOGD(TAG, "stream on socket %d: send failed; closing it", s->fd);
        drop(s);
        return false;
    }
    httpd_sess_update_lru_counter(hd, s->fd);   /* a live stream isn't the one to purge */
    return true;
}

typedef struct {
    char *buf;
    size_t len, cap;
    bool failed;
} text_t;

static void append(text_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void append(text_t *t, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2 && !t->failed; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(t->buf ? t->buf + t->len : NULL, t->buf ? t->cap - t->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            t->failed = true;
        } else if (t->buf != NULL && t->len + (size_t)n < t->cap) {
            t->len += (size_t)n;
            return;
        } else {
            const size_t cap = (t->len + (size_t)n + 1) * 3 / 2 + 64;
            char *b = realloc(t->buf, cap);
            if (b == NULL) {
                t->failed = true;
            } else {
                t->buf = b;
                t->cap = cap;
            }
        }
    }
}

static void add_happening(uint16_t seq, events_mask_t bit, const char *json, void *ctx)
{
    append(ctx, "event: %s\nid: %u\ndata: %s\n\n", events_name(bit), (unsigned)seq, json);
}

/* The happenings `s` hasn't had, in `mask` */
static void send_happenings(stream_t *s, events_mask_t mask)
{
    text_t t = { 0 };
    s->seq = events_since(s->seq, mask, add_happening, &t, NULL);
    if (t.len > 0 && !t.failed) {
        send_all(s, t.buf, t.len);
    }
    free(t.buf);
}

static void flush(void *arg)
{
    (void)arg;
    __atomic_store_n(&s_queued, false, __ATOMIC_SEQ_CST);
    const events_mask_t dirty = events_take(s_sink);
    for (events_mask_t rest = dirty; rest != 0; rest &= rest - 1) {
        const events_mask_t bit = rest & -rest;
        bool anyone = false;
        for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
            anyone |= s_streams[i].fd >= 0 && (s_streams[i].want & bit);
        }
        cJSON *o = anyone ? events_state_json(bit) : NULL;
        char *json = o != NULL ? cJSON_PrintUnformatted(o) : NULL;
        cJSON_Delete(o);
        if (json == NULL) {
            continue;
        }
        const uint32_t h = hash_of(json);
        const int k = __builtin_ctz(bit);
        if (h == s_sent[k]) {
            cJSON_free(json);
            continue;
        }
        s_sent[k] = h;
        text_t t = { 0 };
        append(&t, "event: %s\ndata: %s\n\n", events_name(bit), json);
        cJSON_free(json);
        for (size_t i = 0; !t.failed && i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
            if (s_streams[i].fd >= 0 && (s_streams[i].want & bit)) {
                send_all(&s_streams[i], t.buf, t.len);
            }
        }
        free(t.buf);
    }
    const events_mask_t happenings = ~events_states();
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        if (s_streams[i].fd >= 0) {
            send_happenings(&s_streams[i], s_streams[i].want & happenings);
        }
    }
}

static void queue_flush(void *ctx)
{
    (void)ctx;
    httpd_handle_t hd = web_server_handle();
    if (hd != NULL && !__atomic_exchange_n(&s_queued, true, __ATOMIC_SEQ_CST) &&
        httpd_queue_work(hd, flush, NULL) != ESP_OK) {
        __atomic_store_n(&s_queued, false, __ATOMIC_SEQ_CST);
    }
}

/* What the listener wants: every open stream's kinds */
static void update_want(void)
{
    events_mask_t want = 0;
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        if (s_streams[i].fd >= 0) {
            want |= s_streams[i].want;
        }
    }
    if (s_sink < 0) {
        s_sink = events_listen(want, queue_flush, NULL);
    } else {
        events_want(s_sink, want);
    }
}

/* ------------------------------------------------------------------ heartbeat */

static void heartbeat_send(void *arg)
{
    (void)arg;
    const events_mask_t sys = events_bit("system");
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        if (s_streams[i].fd >= 0 && !(s_streams[i].want & sys)) {
            send_all(&s_streams[i], ":\n\n", 3);
        }
    }
}

static void heartbeat(void *arg)
{
    (void)arg;
    events_changed("system");       /* those that want it get it; the rest a comment */
    httpd_handle_t hd = web_server_handle();
    if (hd != NULL) {
        httpd_queue_work(hd, heartbeat_send, NULL);
    }
}

/* ------------------------------------------------------------------ the route */

typedef struct {
    cJSON *events;
} history_t;

static void add_json(uint16_t seq, events_mask_t bit, const char *json, void *ctx)
{
    (void)seq;
    (void)bit;
    cJSON *e = cJSON_Parse(json);
    if (e != NULL) {
        cJSON_AddItemToArray(((history_t *)ctx)->events, e);
    }
}

static bool wants_stream(httpd_req_t *req)
{
    char v[96];
    if (web_query(req, "format", v, sizeof(v))) {
        return strcmp(v, "json") != 0;
    }
    return httpd_req_get_hdr_value_str(req, "Accept", v, sizeof(v)) != ESP_ERR_NOT_FOUND &&
           strstr(v, "text/event-stream") != NULL;
}

esp_err_t web_events_get(httpd_req_t *req)
{
    char list[160], bad[16] = "";
    events_mask_t want = events_all();
    if (web_query(req, "kinds", list, sizeof(list)) && !events_parse(list, &want, bad, sizeof(bad))) {
        return web_send_error(req, 400, "bad_request", "no such kind of event: %s", bad);
    }
    /* Where to start: after the Last-Event-ID an EventSource sends back, or ?after= */
    char num[12];
    bool replay = httpd_req_get_hdr_value_str(req, "Last-Event-ID", num, sizeof(num)) == ESP_OK ||
                  web_query(req, "after", num, sizeof(num));
    char *end = NULL;
    const unsigned long after = replay ? strtoul(num, &end, 10) : 0;
    if (replay && (end == num || *end != '\0' || after > 65535)) {
        return web_send_error(req, 400, "bad_request", "after: a number from 0 to 65535");
    }

    if (!wants_stream(req)) {
        history_t h = { .events = cJSON_CreateArray() };
        uint32_t lost = 0;
        const uint16_t seq = events_since((uint16_t)after, want & ~events_states(), add_json, &h, &lost);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", seq);
        cJSON_AddNumberToObject(o, "lost", lost);
        cJSON_AddItemToObject(o, "events", h.events);
        return web_send_json(req, 200, o);
    }

    stream_t *s = NULL;
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS && s == NULL; i++) {
        s = s_streams[i].fd < 0 ? &s_streams[i] : NULL;
    }
    if (s == NULL) {
        return web_send_error(req, 503, "busy", "%d event streams are open already", CONFIG_WEB_SERVER_MAX_STREAMS);
    }
    char origin[128];
    char head[320];
    const bool cors = web_cors_origin(req, origin, sizeof(origin));
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/event-stream\r\n"
             "Cache-Control: no-store\r\n"
             "%s%s%s"
             "\r\n"
             "retry: 3000\n\n",
             cors ? "Access-Control-Allow-Origin: " : "", cors ? origin : "", cors ? "\r\nVary: Origin\r\n" : "");
    const int fd = httpd_req_to_sockfd(req);
    if (httpd_socket_send(req->handle, fd, head, strlen(head), 0) != (int)strlen(head)) {
        return ESP_FAIL;
    }
    *s = (stream_t){ .fd = fd, .want = want, .seq = replay ? (uint16_t)after : events_seq() };
    memset(s_sent, 0, sizeof(s_sent));     /* whatever changes next, the new stream hears */
    update_want();
    if (replay) {
        send_happenings(s, want & ~events_states());
    }
    ESP_LOGI(TAG, "event stream on socket %d", fd);
    return ESP_OK;      /* the reply goes on; the server moves on */
}

/* ------------------------------------------------------------------ lifecycle */

void web_events_init(void)
{
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        s_streams[i].fd = -1;
    }
    events_declare("system", system_json);
    web_server_add_feature("events");
}

void web_events_started(void)
{
    if (s_heartbeat == NULL) {
        const esp_timer_create_args_t args = { .callback = heartbeat, .name = "web_events" };
        if (esp_timer_create(&args, &s_heartbeat) != ESP_OK) {
            return;
        }
    }
    esp_timer_start_periodic(s_heartbeat, HEARTBEAT_US);
}

void web_events_stopped(void)
{
    if (s_heartbeat != NULL) {
        esp_timer_stop(s_heartbeat);
    }
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        s_streams[i].fd = -1;
    }
    update_want();
}

void web_events_closed(int fd)
{
    for (size_t i = 0; i < CONFIG_WEB_SERVER_MAX_STREAMS; i++) {
        if (s_streams[i].fd == fd) {
            s_streams[i].fd = -1;
            update_want();
            ESP_LOGI(TAG, "event stream on socket %d closed", fd);
        }
    }
}
