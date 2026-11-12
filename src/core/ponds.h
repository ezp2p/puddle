#ifndef PUDDLE_PONDS_H
#define PUDDLE_PONDS_H

/* The Ponds this Puddle has joined (docs/ponds.md): where each one is, what it has told us (its name, channels and members),
 * how far through its events we are, and the newest messages of each channel. Nothing else of a Pond is kept here: older
 * history is asked of the Pond when the owner scrolls back to it.
 *
 * Kept in <data_dir>/ponds/<pond id>/:
 *   pond.json        the Pond as it last told us (the shape of its /info answer), with its address, its certificate and our
 *                    cursor in its events
 *   members.json     its members
 *   <channel>.jsonl  the newest messages of a channel, oldest first, at most PUDDLE_POND_CACHE
 *
 * Everything a Pond sends is cleaned before it is kept (names, topics, text, profiles), so what is kept is safe to show, and a
 * Pond that sends something malformed changes nothing. Safe to call from several threads. */

#include "base/json.h"
#include "core/identity.h"
#include "core/link.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_POND_CACHE 200  /* messages kept per channel */
#define PUDDLE_POND_TEXT_MAX 4000
#define PUDDLE_POND_FILES_MAX 10   /* files a message carries */
#define PUDDLE_POND_FILE_ID_LEN 64 /* hex, the same id a file has in a Puddle (storage/files.h) */

typedef struct puddle_ponds puddle_ponds;

/* How a member's profile is cleaned before it is kept: puddle_profile_wire_clean, from the layer above (0, or -1 if it is not a
 * profile at all). NULL keeps no profiles. */
typedef int (*puddle_pond_profile_clean)(const char *json, size_t len, char *out, size_t cap, char *notes, size_t notes_cap);

puddle_ponds *puddle_ponds_open(const char *data_dir, puddle_pond_profile_clean clean, char *err, size_t errlen);
void puddle_ponds_close(puddle_ponds *p);

/* Every Pond's id (malloc'd; free() it), and how many. 0 or -1. */
int puddle_ponds_ids(puddle_ponds *p, char (**ids)[PUDDLE_USER_ID_LEN + 1], size_t *n);

/* What is needed to talk to a Pond. */
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_address address;
    char tls_fp[65];
    int64_t cursor; /* the last event applied */
} puddle_pond_conn;

/* 0, or -1 if we have not joined `id`. `out` may be NULL, to ask only that. */
int puddle_ponds_conn(puddle_ponds *p, const char *id, puddle_pond_conn *out);

/* A Pond just joined: `info` is its answer (pond.yaml, Info), `tls_fp` the certificate it showed. Kept under the id it gives,
 * which is copied to `id_out`. Joining one we are in already updates it. 0, or -1 if the answer is malformed. */
int puddle_ponds_add(puddle_ponds *p, const puddle_address *addr, const char *tls_fp, const char *info, size_t len,
                     char id_out[PUDDLE_USER_ID_LEN + 1]);

/* A fresh /info answer (at start, or after a reset): the Pond and its channels are replaced. The messages of a channel that is
 * gone are forgotten. With `reset`, so is every channel's, since what came between is unknown, and the cursor moves to the
 * Pond's newest; without, it stays, so the events since are still applied. 0 or -1. */
int puddle_ponds_info(puddle_ponds *p, const char *id, const char *info, size_t len, int reset);

/* A /members answer: replaces who is in it. 0 or -1. */
int puddle_ponds_members(puddle_ponds *p, const char *id, const char *json, size_t len);

/* A page of history, newest first as the Pond gives it, for `asked` messages before `before` (0 = the newest). Kept if it is
 * the newest, or joins onto the oldest message held; fewer than asked means it reaches the channel's start. 0 or -1. */
int puddle_ponds_history(puddle_ponds *p, const char *id, int64_t channel, int64_t before, const char *json, size_t len, int asked);

/* A message the owner just sent (the Pond's answer to a send): into the cache now, rather than when its event comes back. */
int puddle_ponds_sent(puddle_ponds *p, const char *id, const char *json, size_t len);

/* An /events answer. 1 if it says to start again (fetch /info, members and history afresh), 0 applied, -1 malformed. */
int puddle_ponds_events(puddle_ponds *p, const char *id, const char *json, size_t len);

/* Channels whose newest messages are not held yet (just joined, a new channel, after a reset): malloc'd ids; free() them. */
int puddle_ponds_unfilled(puddle_ponds *p, const char *id, int64_t **channels, size_t *n);

/* How we are getting on with it: "connecting", "connected", "unreachable", "removed" (it no longer knows us),
 * "certificate_changed". Shown to the owner. */
void puddle_ponds_set_status(puddle_ponds *p, const char *id, const char *status);

/* Forget a Pond, and everything kept of it. 0, or -1 if there was none. */
int puddle_ponds_remove(puddle_ponds *p, const char *id);

/* A number that changes whenever anything kept here does: the app polls it to know when to look again. */
uint64_t puddle_ponds_version(puddle_ponds *p);

/* ---- files (docs/ponds.md, "Files") ---- */

/* The largest file the Pond takes, as its /info says: 0 if it has turned files off, -1 if it says nothing (it takes none). */
int64_t puddle_ponds_files_max(puddle_ponds *p, const char *id);

/* Every file the kept messages carry, newest message first: `fn` per file until it returns nonzero. 0, or -1 no such Pond. */
typedef int (*puddle_pond_file_fn)(void *user, int64_t channel, int64_t message, const char *file, int64_t size);
int puddle_ponds_files(puddle_ponds *p, const char *id, puddle_pond_file_fn fn, void *user);

/* Does kept message `message` of `channel` carry `file`? 0 with its size, 1 not kept or not carried, -1 no such channel. */
int puddle_ponds_message_file(puddle_ponds *p, const char *id, int64_t channel, int64_t message, const char *file, int64_t *size);

/* Something the owner sees of the Pond changed outside what is kept here (a file arrived): the version moves on. */
void puddle_ponds_touch(puddle_ponds *p, const char *id);

/* How a message's files are written for the owner: one file (`json`, as kept) to `w`. The owner's routes say what is here of
 * each (objects/file.hpp); NULL writes them as kept. */
typedef struct {
    void (*write)(void *user, puddle_jw *w, int64_t channel, int64_t message, const char *json, size_t len);
    void *user;
} puddle_pond_file_writer;

/* ---- roles (the Pond's docs/design.md, "Roles") ---- */

/* The open offer of a role the Pond made us (its id), or 0. */
int64_t puddle_ponds_offer(puddle_ponds *p, const char *id);
/* The owner answered it: the offer is gone, and our role is `role` (NULL: unchanged). */
void puddle_ponds_answered(puddle_ponds *p, const char *id, const char *role);

/* ---- for the owner's routes (openapi.yaml, "ponds") ---- */

/* [{id, name, description, address, status, channels: [...]}] */
void puddle_ponds_write_list(puddle_ponds *p, puddle_jw *w);
/* {id, name, ..., channels, members, me}. 0, or 1 if there is no such Pond. */
int puddle_ponds_write_one(puddle_ponds *p, const char *id, puddle_jw *w);
/* Is `channel` one of the Pond's channels? */
int puddle_ponds_has_channel(puddle_ponds *p, const char *id, int64_t channel);
/* Up to `limit` messages of `channel` before `before` (0 = the newest), oldest first, as {"items": [...], "more": bool}, if what
 * is kept can answer: 0. 1 if the Pond must be asked (older than what is kept); -1 no such Pond or channel. */
int puddle_ponds_write_messages(puddle_ponds *p, const char *id, int64_t channel, int64_t before, int limit, puddle_jw *w,
                                const puddle_pond_file_writer *files);
/* Write a page of history as the Pond gave it (newest first) in the same shape, oldest first. 0 or -1 if malformed. */
int puddle_ponds_write_page(const char *json, size_t len, int asked, puddle_jw *w, const puddle_pond_file_writer *files);

#ifdef __cplusplus
}
#endif
#endif
