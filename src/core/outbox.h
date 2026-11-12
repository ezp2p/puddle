#ifndef PUDDLE_OUTBOX_H
#define PUDDLE_OUTBOX_H

/* Things this Puddle owes other Puddles, kept on disk until they are delivered (docs/roadmap.md 3.4): a stream
 * request, the answer to one, a notice that a stream is closed. Each is a signed POST to a peer.
 *
 * What is retried and what is not is a rule the owner chose:
 *   - the other Puddle is OFFLINE (refused, timed out, unreachable, not found): try again every five minutes
 *     for the first 24 hours of its being away, then every thirty minutes, for as long as it takes. The check
 *     is the delivery itself, tried on the oldest waiting item; once it gets through, everything else for that
 *     peer follows at once. The schedule is saved, so it survives a restart.
 *   - for a peer whose key we hold, only an answer THEY SIGNED counts (docs/streams.md, "Proving who answers"). Before
 *     a request on a route that needs a stream there is a handshake (`GET /stream/v1/hello`), unless a certificate was
 *     proven for them in the last ten minutes, and the request goes only over a connection showing it (the sender does
 *     this: federation/sender.h). An answer they did not sign for, something that isn't a Puddle, someone in between:
 *     nothing was delivered, so it is treated like OFFLINE, with the reason saved on every item waiting for them so
 *     the app can say what happened. Every address we hold for them is tried first.
 *   - a SIGNED REFUSAL (any status but 2xx, in their name) fails once and is never retried. So does anything but an
 *     answer when we hold no key for them (a first request to an address): there is nobody to prove.
 *     The item is kept, with the reason, so the app can say "this failed because...".
 * Items for one peer go out in the order they were queued, one at a time, except that news of a key change
 * (kind "identity") goes first: whatever else is waiting is signed by the new key, and the other side has to be told
 * about it before it will believe them. Different peers are delivered to
 * side by side (each on its own thread, up to PUDDLE_OUTBOX_PARALLEL), so a slow or dead peer holds up only
 * itself. The hooks below are therefore called from several threads and must be safe for that. */

#include "core/delivery.h"
#include "core/link.h"
#include "storage/storage.h"
#include "core/identity.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the kind of item that asks an address for a stream; its `peer` is the made-up id of a provisional record */
#define PUDDLE_OB_KIND_OPEN_ACCESS "open_access"

#define PUDDLE_OUTBOX_MAX_QUEUED 1000
#define PUDDLE_OUTBOX_MAX_FAILED 200
#define PUDDLE_OUTBOX_BODY_MAX (128u * 1024)
#define PUDDLE_OUTBOX_PARALLEL 8    /* peers delivered to at the same time */

typedef enum { PUDDLE_OB_QUEUED, PUDDLE_OB_FAILED } puddle_ob_state;

typedef struct {
    uint64_t seq;
    char peer[PUDDLE_USER_ID_LEN + 1];
    char kind[32];             /* what the caller calls it ("access", "result", ...); handed back on `done` */
    char method[8];            /* "POST" (the default) or "GET" */
    char path[320];            /* the endpoint; a GET may carry a query ("?hash=...", a file's "?chat=...&message=...") */
    size_t max_reply;          /* the largest answer to take, for a GET that fetches a file; 0 = the usual */
    char *body;                /* a JSON object, NUL-terminated */
    size_t body_len;
    int64_t created_ms, next_ms, first_offline_ms;
    int attempts;
    puddle_ob_state state;
    int status;                /* the last HTTP status seen, 0 if none */
    char reason[200];          /* why it is waiting or failed, in words */
} puddle_ob_item;

typedef enum {
    PUDDLE_OB_DELIVERED,  /* a 2xx came back (in their name, if we hold their key) */
    PUDDLE_OB_REJECTED    /* failed for good; the item stays, marked failed, with its reason */
} puddle_ob_outcome;

/* How long one delivery may take, in seconds; 0 = the sender's default. */
typedef struct {
    int connect_timeout_s;
    int io_timeout_s;
    int total_timeout_s;
} puddle_ob_timeouts;

typedef struct puddle_outbox_hooks puddle_outbox_hooks;
struct puddle_outbox_hooks {
    /* Send one item to its peer and say what came of it in `reply`. The outbox only queues, schedules and keeps
     * count; how an item travels is this hook's (federation/sender.h: puddle_federation_send). NULL: nothing is
     * ever sent, and each attempt fails. */
    void (*send)(const puddle_outbox_hooks *hooks, const puddle_ob_item *item, puddle_net_reply *reply);
    puddle_identity *identity;                                   /* signs everything */
    /* where to reach `peer`, best first; returns how many (0 = we do not know where) */
    int (*addresses)(void *user, const char *peer, puddle_address *out, int max);
    /* the current signing key we hold for `peer`: 1 and `sign_pub` filled, or 0 if we hold none (then nothing is checked) */
    int (*peer_key)(void *user, const char *peer, unsigned char sign_pub[32]);
    /* An item reached an end. Called without the outbox's lock, so it may queue more. `reply` may be NULL. */
    void (*done)(void *user, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply);
    void *user;
    int64_t (*now_ms)(void);                                     /* NULL = the clock */
    puddle_ob_timeouts net;                                      /* for the sender */
    int max_queued, max_failed;                                  /* 0 = the defaults above; tests use small ones */
};

typedef struct puddle_outbox puddle_outbox;

/* Read what is waiting from <data_dir>/state/outbox/. A damaged file is skipped with a warning. */
puddle_outbox *puddle_outbox_open(const char *data_dir, const puddle_outbox_hooks *hooks, char *err, size_t errlen);
/* As puddle_outbox_open, kept in any storage, which must outlive it. */
puddle_outbox *puddle_outbox_open_with(puddle_storage *storage, const puddle_outbox_hooks *hooks, char *err, size_t errlen);
void puddle_outbox_close(puddle_outbox *ob); /* stops the worker first */

/* Start (and stop, via close) a thread that delivers things as they fall due. */
int puddle_outbox_start(puddle_outbox *ob);

/* Queue a POST of `body` (a JSON object) to `path` on `peer`, to go out at once. 0, or -1 with a reason. */
int puddle_outbox_enqueue(puddle_outbox *ob, const char *peer, const char *kind, const char *path, const char *body,
                          size_t body_len, char *err, size_t errlen);

/* Queue a GET of `path` (with its query) on `peer`, to fetch something: what comes back reaches the `done` hook in the reply, and
 * nothing larger than `max_reply` bytes is taken. Otherwise as puddle_outbox_enqueue. */
int puddle_outbox_enqueue_get(puddle_outbox *ob, const char *peer, const char *kind, const char *path, size_t max_reply, char *err,
                              size_t errlen);

/* One pass: deliver what is due. The worker calls this; tests call it directly. */
void puddle_outbox_poll(puddle_outbox *ob);

/* Change what a waiting item will say (a key chain or a signature inside it has gone out of date). Only for an item
 * that is still waiting. An attempt already under way carries on with what it had. Returns 0, 1 if there is no such item or it is
 * not in a state to change, -1 on failure. */
int puddle_outbox_rewrite(puddle_outbox *ob, uint64_t seq, const char *body, size_t body_len);

/* Try this peer's waiting items now, not at the next scheduled time. Returns how many were waiting. */
int puddle_outbox_retry_now(puddle_outbox *ob, const char *peer);

/* Give an item that failed another go: it is queued again, to be tried now, as if new. 0, or 1 if there is no such failed item. */
int puddle_outbox_requeue(puddle_outbox *ob, uint64_t seq);

/* Everything in the outbox, oldest first (a copy: free with puddle_outbox_list_free). */
int puddle_outbox_list(puddle_outbox *ob, puddle_ob_item **out, size_t *count);
void puddle_outbox_list_free(puddle_ob_item *items, size_t count);

/* Remove an item that failed, once the owner has seen it. 0, 1 no such item, -1. */
int puddle_outbox_dismiss(puddle_outbox *ob, uint64_t seq);

/* How long to wait before trying an offline peer again, given when it was first found offline. */
int64_t puddle_outbox_retry_delay_ms(int64_t first_offline_ms, int64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
