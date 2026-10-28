#ifndef PUDDLE_TRANSFERS_H
#define PUDDLE_TRANSFERS_H

/* Fetching files other Puddles sent us (docs/files.md), a queue of its own beside the outbox. Each download names the
 * file, the message that carries it (in a chat), and who to ask, in order: the sender first, then anyone else in the
 * chat. It gets the file's chunk list, checks it against the id, then fetches one chunk at a time, each checked as it
 * arrives (storage/files.h), from the first who has it. When nobody can be reached it waits and tries again, longer each
 * time, and gives up after a week.
 *
 * One worker thread runs one download at a time. Once an hour it also clears up (storage/files.h): files no message has
 * carried for a day, and uploads left unfinished for a day. Downloads are kept in <data_dir>/state/transfers/<id>.json, so they
 * carry on after a restart. How a request travels is the `get` hook's (federation/files.h). */

#include "core/delivery.h"
#include "storage/files.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_TRANSFER_SOURCES 8

typedef enum {
    PUDDLE_TR_NONE,    /* no download of this file */
    PUDDLE_TR_QUEUED,  /* waiting its turn, or to try again */
    PUDDLE_TR_ACTIVE,  /* being fetched now */
    PUDDLE_TR_FAILED   /* given up; start it again to retry */
} puddle_tr_state;

typedef struct {
    /* GET `path` (a signed stream request) from `peer`, taking an answer of up to `max` bytes. Called on the worker thread. */
    void (*get)(void *user, const char *peer, const char *path, size_t max, puddle_net_reply *reply);
    void *user;
    int64_t (*now_ms)(void); /* NULL = the clock */
    /* A download is here whole (may be NULL). Called on the worker thread, without the queue's lock. */
    void (*finished)(void *user, const char *id, const char *chat, const char *message);
    void *finished_user;
} puddle_transfer_hooks;

typedef struct puddle_transfers puddle_transfers;

/* Open the queue over the file store `files` (which outlives it). The worker starts only if `hooks->get` is set. NULL with
 * a message in `err` if the folder cannot be made. */
puddle_transfers *puddle_transfers_open(const char *data_dir, puddle_files *files, const puddle_transfer_hooks *hooks, char *err, size_t errlen);
void puddle_transfers_close(puddle_transfers *t); /* stops the worker; a download in progress carries on at the next open */

/* Fetch the file `id` of `size` bytes, carried by `message` in `chat`, from `sources` in that order (up to 8). Already
 * queued: the new sources are added at the end. Failed: tried again from the start. Already here whole: nothing to do.
 * 0, or -1. */
int puddle_transfers_start(puddle_transfers *t, const char *id, int64_t size, const char *chat, const char *message,
                           const char *const *sources, size_t nsources);

/* Stop fetching it and throw away what came. 0 (also if there was no such download). */
int puddle_transfers_cancel(puddle_transfers *t, const char *id);

/* How a download stands; `reason` (200 bytes, may be NULL) says why it is waiting or failed, "" if not. */
puddle_tr_state puddle_transfers_state(puddle_transfers *t, const char *id, char *reason);

/* Try every waiting download now, rather than when its wait is over (someone came back online). */
void puddle_transfers_retry_now(puddle_transfers *t);

/* No message carries the file any more: stop fetching it, and throw it away whole or partial. */
void puddle_transfers_forget(puddle_transfers *t, puddle_files *files, const char *id);

/* How long to wait before the next try, after `attempts` failed ones. */
int64_t puddle_transfer_retry_delay_ms(int attempts);

#ifdef __cplusplus
}
#endif
#endif
