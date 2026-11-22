/*
 * events: what changed, for whoever is listening. See events.h.
 *
 * One mutex covers the kinds, the listeners and the ring. Nothing that might block or take
 * another component's lock -- a builder, a listener's notify -- runs while it is held, but
 * events_since()'s and events_next()'s callback, which only copies, and the printing of a
 * happening's text (a hundred bytes or so), so that it is counted and kept at once. It is
 * recursive, so that callback may still ask for a kind's name.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "events.h"

typedef struct {
    const char *name;
    events_build_fn_t build;    /* NULL: a happening */
} kind_t;

typedef struct {
    bool used;
    events_mask_t want;
    events_mask_t dirty;        /* state kinds changed since the last take */
    events_notify_fn_t notify;
    void *ctx;
} sink_t;

typedef struct {
    uint32_t n;                 /* which happening, counting from 1; 0 for an empty slot */
    events_mask_t bit;
    char *json;
} kept_t;

static kind_t s_kinds[EVENTS_MAX_KINDS];
static size_t s_n_kinds;
static sink_t s_sinks[EVENTS_MAX_SINKS];
static kept_t s_ring[EVENTS_KEPT];
static uint32_t s_count;        /* happenings so far */

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/* Before app_main, so a source may publish before anyone declares or listens. */
__attribute__((constructor)) static void events_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
}

static void lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

/* seq for the n-th happening: 1..65535, then round again */
static uint16_t seq_of(uint32_t n)
{
    return n == 0 ? 0 : (uint16_t)((n - 1) % 65535 + 1);
}

/* With the lock held: the kind's index, or -1 */
static int find(const char *name)
{
    for (size_t i = 0; i < s_n_kinds; i++) {
        if (strcmp(s_kinds[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int index_of(events_mask_t bit)
{
    return bit == 0 || (bit & (bit - 1)) != 0 ? -1 : __builtin_ctz(bit);
}

esp_err_t events_declare(const char *name, events_build_fn_t build)
{
    if (name == NULL || strlen(name) > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    lock();
    int i = find(name);
    if (i < 0 && s_n_kinds < EVENTS_MAX_KINDS) {
        i = (int)s_n_kinds++;
        s_kinds[i].name = name;
    }
    if (i >= 0) {
        s_kinds[i].build = build;
    } else {
        err = ESP_ERR_NO_MEM;
    }
    unlock();
    return err;
}

events_mask_t events_bit(const char *name)
{
    lock();
    const int i = find(name);
    unlock();
    return i < 0 ? 0 : (events_mask_t)1 << i;
}

events_mask_t events_all(void)
{
    lock();
    const events_mask_t all = s_n_kinds >= 32 ? ~(events_mask_t)0 : ((events_mask_t)1 << s_n_kinds) - 1;
    unlock();
    return all;
}

events_mask_t events_states(void)
{
    events_mask_t m = 0;
    lock();
    for (size_t i = 0; i < s_n_kinds; i++) {
        if (s_kinds[i].build != NULL) {
            m |= (events_mask_t)1 << i;
        }
    }
    unlock();
    return m;
}

const char *events_name(events_mask_t bit)
{
    const int i = index_of(bit);
    lock();
    const char *name = i >= 0 && (size_t)i < s_n_kinds ? s_kinds[i].name : NULL;
    unlock();
    return name;
}

bool events_parse(const char *list, events_mask_t *out, char *bad, size_t bad_len)
{
    events_mask_t m = 0;
    const char *p = list;
    while (p != NULL && *p) {
        const char *comma = strchr(p, ',');
        const size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0) {
            char name[16];
            bool found = false;
            if (len < sizeof(name)) {
                memcpy(name, p, len);
                name[len] = '\0';
                const events_mask_t bit = events_bit(name);
                found = bit != 0;
                m |= bit;
            }
            if (!found) {
                if (bad != NULL && bad_len > 0) {
                    const size_t n = len < bad_len - 1 ? len : bad_len - 1;
                    memcpy(bad, p, n);
                    bad[n] = '\0';
                }
                return false;
            }
        }
        p = comma ? comma + 1 : NULL;
    }
    *out = m ? m : events_all();
    return true;
}

bool events_wanted(const char *name)
{
    bool wanted = false;
    lock();
    const int i = find(name);
    for (size_t s = 0; i >= 0 && s < EVENTS_MAX_SINKS; s++) {
        wanted |= s_sinks[s].used && (s_sinks[s].want & ((events_mask_t)1 << i));
    }
    unlock();
    return wanted;
}

/* Tell the listeners that want `bit` (outside the lock: a notify may queue work) */
static void notify(events_mask_t bit, bool state)
{
    events_notify_fn_t fns[EVENTS_MAX_SINKS];
    void *ctxs[EVENTS_MAX_SINKS];
    size_t n = 0;
    lock();
    for (size_t s = 0; s < EVENTS_MAX_SINKS; s++) {
        if (s_sinks[s].used && (s_sinks[s].want & bit)) {
            if (state) {
                s_sinks[s].dirty |= bit;
            }
            fns[n] = s_sinks[s].notify;
            ctxs[n++] = s_sinks[s].ctx;
        }
    }
    unlock();
    for (size_t i = 0; i < n; i++) {
        if (fns[i] != NULL) {
            fns[i](ctxs[i]);
        }
    }
}

void events_changed(const char *name)
{
    const events_mask_t bit = events_bit(name);
    if (bit != 0) {
        notify(bit, true);
    }
}

void events_happened(const char *name, cJSON *fields)
{
    const events_mask_t bit = events_bit(name);
    if (bit == 0) {
        cJSON_Delete(fields);
        return;
    }

    /* Built outside the lock but for its seq, which is set, printed and stored in the same hold
     * as the count is taken: s_count never counts a happening that isn't in the ring yet, so a
     * reader that goes on from the seq it was given can't step over one still being printed */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", name);
    cJSON *seq = cJSON_AddNumberToObject(o, "seq", 0);
    cJSON_AddNumberToObject(o, "t_ms", (double)(esp_timer_get_time() / 1000));
    if (fields != NULL) {
        cJSON *f = fields->child;
        while (f != NULL) {
            cJSON *next = f->next;
            cJSON_AddItemToObject(o, f->string, cJSON_DetachItemViaPointer(fields, f));
            f = next;
        }
        cJSON_Delete(fields);
    }

    lock();
    const uint32_t n = ++s_count;
    cJSON_SetNumberValue(seq, seq_of(n));
    char *json = cJSON_PrintUnformatted(o);
    kept_t *k = &s_ring[n % EVENTS_KEPT];
    char *old = k->json;
    k->n = json != NULL ? n : 0;    /* not printed: counted, never kept, not `lost` */
    k->bit = bit;
    k->json = json;
    unlock();
    cJSON_Delete(o);
    free(old);
    notify(bit, false);
}

uint16_t events_seq(void)
{
    lock();
    const uint16_t seq = seq_of(s_count);
    unlock();
    return seq;
}

cJSON *events_state_json(events_mask_t bit)
{
    const int i = index_of(bit);
    lock();
    const char *name = i >= 0 && (size_t)i < s_n_kinds ? s_kinds[i].name : NULL;
    const events_build_fn_t build = name != NULL ? s_kinds[i].build : NULL;
    unlock();
    if (build == NULL) {
        return NULL;
    }
    cJSON *state = build();
    if (state == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", name);
    cJSON_AddItemToObject(o, "state", state);
    return o;
}

/* With the lock held: the count of the first kept happening after `after` (a seq; 0 for all of
 * them), and in `lost` how many after `after` are no longer kept */
static uint32_t first_after(uint16_t after, uint32_t latest, uint32_t *lost)
{
    const uint32_t oldest = latest > EVENTS_KEPT ? latest - EVENTS_KEPT + 1 : 1;
    /* `after` as a count: how far back it is from the latest, going round 65535 */
    uint32_t from = 0;
    if (after != 0 && latest != 0) {
        const uint32_t back = (uint32_t)((seq_of(latest) - after + 65535) % 65535);
        from = back <= latest ? latest - back : 0;
    }
    if (lost != NULL) {
        *lost = oldest > from + 1 && latest >= oldest ? oldest - (from + 1) : 0;
    }
    return from + 1 > oldest ? from + 1 : oldest;
}

uint16_t events_since(uint16_t after, events_mask_t mask, events_each_fn_t cb, void *ctx, uint32_t *lost)
{
    lock();
    const uint32_t latest = s_count;
    for (uint32_t n = first_after(after, latest, lost); latest != 0 && n <= latest; n++) {
        const kept_t *k = &s_ring[n % EVENTS_KEPT];
        if (k->n == n && (k->bit & mask) && cb != NULL) {
            cb(seq_of(n), k->bit, k->json, ctx);
        }
    }
    const uint16_t seq = seq_of(latest);
    unlock();
    return seq;
}

uint16_t events_open(uint16_t after, events_mask_t mask, events_cursor_t *cursor, uint32_t *lost)
{
    lock();
    const uint32_t latest = s_count;
    *cursor = (events_cursor_t){ .next = first_after(after, latest, lost), .last = latest, .mask = mask, .lost = 0 };
    const uint16_t seq = seq_of(latest);
    unlock();
    return seq;
}

bool events_next(events_cursor_t *cursor, events_each_fn_t cb, void *ctx)
{
    bool found = false;
    lock();
    /* Counts, not seqs: what was published since the open doesn't move the cursor's range. A slot
     * taken by a newer happening meanwhile holds a later count: the one due there was pushed out,
     * whatever its kind, and is counted. An empty slot is one never kept (not printed), skipped
     * as events_since() skips it. */
    for (; !found && cursor->next <= cursor->last; cursor->next++) {
        const kept_t *k = &s_ring[cursor->next % EVENTS_KEPT];
        if (k->n > cursor->next) {
            cursor->lost++;
        } else if (k->n == cursor->next && (k->bit & cursor->mask)) {
            if (cb != NULL) {
                cb(seq_of(cursor->next), k->bit, k->json, ctx);
            }
            found = true;
        }
    }
    unlock();
    return found;
}

int events_listen(events_mask_t want, events_notify_fn_t fn, void *ctx)
{
    int id = -1;
    lock();
    for (int s = 0; s < EVENTS_MAX_SINKS; s++) {
        if (!s_sinks[s].used) {
            s_sinks[s] = (sink_t){ .used = true, .want = want, .notify = fn, .ctx = ctx };
            id = s;
            break;
        }
    }
    unlock();
    return id;
}

void events_want(int id, events_mask_t want)
{
    if (id < 0 || id >= EVENTS_MAX_SINKS) {
        return;
    }
    lock();
    s_sinks[id].want = want;
    s_sinks[id].dirty &= want;
    unlock();
}

void events_unlisten(int id)
{
    if (id < 0 || id >= EVENTS_MAX_SINKS) {
        return;
    }
    lock();
    s_sinks[id] = (sink_t){ 0 };
    unlock();
}

events_mask_t events_take(int id)
{
    if (id < 0 || id >= EVENTS_MAX_SINKS) {
        return 0;
    }
    lock();
    const events_mask_t dirty = s_sinks[id].dirty;
    s_sinks[id].dirty = 0;
    unlock();
    return dirty;
}
