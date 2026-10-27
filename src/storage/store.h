#ifndef PUDDLE_STORE_H
#define PUDDLE_STORE_H

/* Chat storage: one append-only log per chat, split into one file per UTC day.
 * See docs/storage.md. Thread-safe (one lock per store). Nothing here is
 * signed or encrypted yet: `sig` is stored as given and not verified. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_ID_HEX 64 /* event ids and chunk hashes: lowercase hex SHA-256 */

typedef struct {
    char id[PUDDLE_ID_HEX + 1];
    int64_t ts;        /* sender's time, UTC ms */
    int64_t rts;       /* when this server received it, UTC ms */
    char *event;       /* the exact event JSON, NUL-terminated */
    size_t event_len;
    char *sig;         /* may be empty */
} puddle_event;

typedef struct {
    puddle_event *items;
    size_t count;
} puddle_event_list;

/* One federation chunk: all events of a chat on one UTC day. */
typedef struct {
    char day[11];      /* "YYYY-MM-DD" */
    size_t count;
    char hash[PUDDLE_ID_HEX + 1]; /* SHA-256 over the sorted event ids joined by '\n' */
} puddle_chunk;

typedef struct {
    puddle_chunk *items;
    size_t count;
} puddle_chunk_list;

void puddle_event_free(puddle_event *e);
void puddle_event_list_free(puddle_event_list *l);
void puddle_chunk_list_free(puddle_chunk_list *l);

typedef struct puddle_store puddle_store;

/* Create the data directory if needed and lock it. A second open of the same
 * directory (another Puddle running) fails. NULL + message in `err` on failure. */
puddle_store *puddle_store_open(const char *data_dir, char *err, size_t errlen);
void puddle_store_close(puddle_store *s);

enum { PUDDLE_APPEND_NEW = 0, PUDDLE_APPEND_DUPLICATE = 1 };

/* Append an event to a chat. `event` must be one compact JSON object (no line breaks) with a
 * string "chat" equal to `chat_id` and an integer "ts" (UTC ms, years 2000-2100). `sig` may be
 * NULL. `rts_ms` 0 means "now". The event is stored byte-for-byte and its id is the SHA-256 of
 * those bytes, written to `id_out` (65 bytes). It is flushed to disk before returning.
 * Returns PUDDLE_APPEND_NEW, PUDDLE_APPEND_DUPLICATE (already stored, nothing written) or -1. */
int puddle_store_append(puddle_store *s, const char *chat_id, const char *event, size_t event_len,
                        const char *sig, int64_t rts_ms, char id_out[PUDDLE_ID_HEX + 1],
                        char *err, size_t errlen);

/* Be told of every new event (not duplicates), after it is on disk and with no lock held: `fn(user, chat_id)`. One
 * listener; set it before other threads use the store. NULL stops it. The conversation summaries use this to know
 * when theirs is out of date. */
typedef void (*puddle_store_appended_fn)(void *user, const char *chat_id);
void puddle_store_on_append(puddle_store *s, puddle_store_appended_fn fn, void *user);

/* The last `n` events of a chat in chat order (ts, then id), oldest first. Reads only the newest
 * day files; does not load the whole chat. An unknown chat gives an empty list. 0 or -1. */
int puddle_store_tail(puddle_store *s, const char *chat_id, size_t n, puddle_event_list *out,
                      char *err, size_t errlen);

/* Fetch one event by id. 0 found, 1 not found, -1 error. Free with puddle_event_free(). */
int puddle_store_get(puddle_store *s, const char *chat_id, const char *id, puddle_event *out,
                     char *err, size_t errlen);

/* Per-day chunk summaries for federation sync, oldest first. 0 or -1. */
int puddle_store_chunks(puddle_store *s, const char *chat_id, puddle_chunk_list *out,
                        char *err, size_t errlen);

/* How many chats are currently held in memory (for tests and diagnostics). */
size_t puddle_store_loaded_chats(puddle_store *s);

#ifdef __cplusplus
}
#endif
#endif
