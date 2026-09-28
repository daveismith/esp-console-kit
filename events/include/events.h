/*
 * events -- what changed on the board, for whoever is listening: the web server's event stream
 * (web_server, GET /api/v1/events), and a host link. Nothing here is HTTP. See events.c.
 *
 * Two sorts of event, each a kind with a name:
 *
 *  - A state kind ("screen", "leds", ...) is a resource that changed. Its owner registers a
 *    builder -- the JSON its GET returns -- and calls events_changed() when it changes. Nothing
 *    is built then: a listener is marked, and builds the latest state when it sends, so a burst
 *    of changes is one event. State is not kept: a listener that missed some reads it again.
 *
 *  - A happening ("clip_ended", "scene_ended", ...) is something that occurred. events_happened()
 *    numbers it (seq, 1..65535, then 1 again), stamps it (t_ms, uptime) and keeps it in a ring of
 *    the last EVENTS_KEPT, so a listener that missed some can catch up.
 *
 * On the wire an event is one JSON object:
 *   {"event":"leds","state":{...the GET's JSON...}}
 *   {"event":"scene_ended","seq":42,"t_ms":812345,"name":"message",...}
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENTS_MAX_KINDS    16
#define EVENTS_MAX_SINKS    4
#define EVENTS_KEPT         32

typedef uint32_t events_mask_t;     /* a bit a kind */

/* The JSON a state kind's GET returns; the caller deletes it. */
typedef cJSON *(*events_build_fn_t)(void);

/* Declare a kind: a state kind with its builder, or (build NULL) a happening. Names are string
 * literals, at most 15 characters. Declaring one again replaces its builder. */
esp_err_t events_declare(const char *name, events_build_fn_t build);

/* The kind's bit, or 0 for a name not declared. */
events_mask_t events_bit(const char *name);
/* Every kind declared, and every state kind. */
events_mask_t events_all(void);
events_mask_t events_states(void);
/* The name of the kind with this bit (one bit), or NULL. */
const char *events_name(events_mask_t bit);

/* A comma-separated list of names ("screen,leds") as a mask. False, with `bad` holding the first
 * name not declared, when there is one. An empty list is every kind. */
bool events_parse(const char *list, events_mask_t *out, char *bad, size_t bad_len);

/* Whether any listener wants this kind now: a source can skip work nobody will see. */
bool events_wanted(const char *name);

/* A state kind changed. Any task; not an ISR. Cheap: no JSON is built. */
void events_changed(const char *name);

/* A happening: `fields` (an object, or NULL) are its own; this takes it. Any task; not an ISR. */
void events_happened(const char *name, cJSON *fields);

/* The latest happening's seq; 0 for none yet. */
uint16_t events_seq(void);

/* The state kind as its event: {"event": name, "state": {...}}. NULL when it has no builder. */
cJSON *events_state_json(events_mask_t bit);

/*
 * The kept happenings in `mask` after `after` (a seq; 0 for all of them), oldest first: `cb` gets
 * each one's seq, kind and JSON text, with the ring locked -- it must only copy. Returns the
 * latest seq, and in `lost` how many after `after` are no longer kept.
 */
typedef void (*events_each_fn_t)(uint16_t seq, events_mask_t bit, const char *json, void *ctx);
uint16_t events_since(uint16_t after, events_mask_t mask, events_each_fn_t cb, void *ctx, uint32_t *lost);

/* ---- listeners ----
 *
 * A listener says which kinds it wants, and is told (`notify`, on whichever task changed
 * something: it must not block) when one of them has. It then takes the state kinds that
 * changed with events_take(), and reads new happenings with events_since(). */

typedef void (*events_notify_fn_t)(void *ctx);

/* A listener's id (>= 0), or -1 when EVENTS_MAX_SINKS are taken. */
int events_listen(events_mask_t want, events_notify_fn_t notify, void *ctx);
void events_want(int id, events_mask_t want);
void events_unlisten(int id);

/* The state kinds changed since the last take, and cleared. */
events_mask_t events_take(int id);

#ifdef __cplusplus
}
#endif
