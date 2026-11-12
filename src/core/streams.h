#ifndef PUDDLE_STREAMS_H
#define PUDDLE_STREAMS_H

/* What this Puddle remembers about the other Puddles it has streams with (docs/streams.md): one small file
 * per peer, the list of blocked ids, the invite tokens it has handed out, and who may ask for a stream at all.
 * Everything lives under <data_dir>/state/ and is replaced atomically, so a crash leaves the old file or the
 * new one. All functions are safe to call from several threads.
 *
 * What goes in is checked on the way: a peer's chain is verified against its id before it is stored, so
 * nothing in here is a key that hasn't been proven to belong to the id it is filed under. */

#include "core/identity.h"
#include "core/link.h"
#include "storage/storage.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_STREAM_MAX_ADDRS 4
#define PUDDLE_NOTE_MAX_BYTES 2000
#define PUDDLE_CHAIN_MAX_BYTES (256u * 1024)
#define PUDDLE_PROFILE_MAX_BYTES (16u * 1024)
#define PUDDLE_MAX_INVITES 64

typedef enum {
    PUDDLE_STREAM_PENDING_OUT, /* we asked, they have not answered */
    PUDDLE_STREAM_PENDING_IN,  /* they asked, the owner has not answered */
    PUDDLE_STREAM_ACTIVE,      /* trusted both ways */
    PUDDLE_STREAM_CLOSED,      /* dried up; either side may ask again */
    PUDDLE_STREAM_SCOPED       /* a group-only connection: group events with them, nothing else (docs/groups.md) */
} puddle_stream_state;

const char *puddle_stream_state_name(puddle_stream_state s);
int puddle_stream_state_parse(const char *name, puddle_stream_state *out); /* 0 or -1 */

/* What we still have to tell the other side about a decision, so it survives a restart and an offline peer.
 * Delivered, and cleared, by the outbox (roadmap 3.4, 3.5). */
typedef enum {
    PUDDLE_OWED_NONE,
    PUDDLE_OWED_APPROVED, /* tell them `access/result` approved */
    PUDDLE_OWED_DECLINED, /* ...declined */
    PUDDLE_OWED_CLOSED,   /* tell them the stream is closed */
    PUDDLE_OWED_SCOPED    /* tell them yes to a group-only connection */
} puddle_owed;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_stream_state state;
    puddle_owed owed;
    char *chain;      /* their verified key chain; NULL until we have seen it (a request we sent, not yet answered) */
    size_t chain_len;
    char addresses[PUDDLE_STREAM_MAX_ADDRS][PUDDLE_ADDR_MAX]; /* where to reach them, best first */
    int naddr;
    int64_t addr_seq;  /* of the signed address record these came from; the highest wins */
    char tls_fp[65];       /* SHA-256 of their certificate as first seen, hex; "" until we have connected to them */
    char tls_fp_seen[65];  /* a different certificate we met since, waiting for the owner to say it is fine; else "" */
    int64_t chain_sent;    /* the highest seq of OUR key chain this peer is known to hold; -1 = not known */
    int64_t addr_sent;     /* the highest seq of OUR address record this peer is known to hold; -1 = not known */
    char ticket[33];       /* 32 hex characters: a request we sent to an address, until its answer names who they are (and the
                            * same ticket on the record of someone who asked us, to be handed back in our answer); "" if none */
    int scoped_req;        /* a request we sent only because we share a group with them: if it is answered yes, the stream is `scoped` */
    int group_ok;          /* we have a group connection with them: group events pass whatever else is true of the stream (friends or not) */
    int provisional;       /* a request we sent to an address whose owner we do not know yet: filed under a made-up id */
    char error[200];       /* why a request we sent, or an answer we owed, did not get through; "" if nothing went wrong */
    char note[PUDDLE_NOTE_MAX_BYTES + 1]; /* the message sent with the request */
    int64_t created_ms, updated_ms;
} puddle_stream;

/* A request to an address, before we know who lives there: it is filed under an id made from a random `ticket`, which
 * the answer must repeat. Both are made here. */
int puddle_ticket_new(char out[33]);
int puddle_provisional_id(const char *ticket, char out[PUDDLE_USER_ID_LEN + 1]);

/* May group events travel over this stream: friends, group-only connections, and anyone we share a group connection with? (A block
 * is checked separately.) */
int puddle_stream_carries_groups(const puddle_stream *st);

/* Fill in an empty record for `id`. 0, or -1 if `id` is not a valid user id. */
int puddle_stream_init(puddle_stream *st, const char *id, puddle_stream_state state, int64_t now_ms);
/* Copy text in (replacing what was there). 0 or -1 (out of memory, or over the size limit). A NULL text clears it. */
int puddle_stream_set_chain(puddle_stream *st, const char *text, size_t len);
/* Add an address to the end (ignores one already present). 0 or -1 if it is invalid or the list is full. */
int puddle_stream_add_address(puddle_stream *st, const char *address);
void puddle_stream_free(puddle_stream *st); /* frees what the record owns; the struct itself is the caller's */

typedef struct puddle_streams puddle_streams;

/* Open and read every stream record in (collection "streams", one per peer; the block list, read markers, invites
 * and access policy are documents in "state"). A damaged record is skipped with a warning rather than stopping the
 * Puddle: it loses that one peer, not all of them. The first form keeps them as files under `data_dir`; the second
 * uses `storage`, which must outlive the store. */
puddle_streams *puddle_streams_open(const char *data_dir, char *err, size_t errlen);
puddle_streams *puddle_streams_open_with(puddle_storage *storage, char *err, size_t errlen);

/* Stream records written before profiles moved to their own records (core/people.h) carry the peer's profile. Hand
 * each such profile to `fn`; where it returns 0, the record is written again without it. Run once at startup. The
 * number handed over, or -1 if a record could not be rewritten. */
typedef int (*puddle_stream_profile_fn)(void *user, const char *id, const char *json, size_t len, int64_t updated_ms);
int puddle_streams_take_profiles(puddle_streams *s, puddle_stream_profile_fn fn, void *user);
void puddle_streams_close(puddle_streams *s);

/* 0 found (a copy: free it with puddle_stream_free), 1 no such peer, -1 `id` is not a valid user id. */
int puddle_streams_get(puddle_streams *s, const char *id, puddle_stream *out);

/* Store a record (new or replacing). Its chain, if any, must verify and belong to `st->id`. Not allowed for a
 * blocked id. Returns 0, or -1 with a reason in `err`. */
int puddle_streams_put(puddle_streams *s, const puddle_stream *st, char *err, size_t errlen);

/* Change one peer's record without anyone else's change to it being lost. Several threads touch a record (the owner
 * accepting, a request arriving, the outbox noting a certificate), and get-then-put would let one overwrite another;
 * this holds the store's lock across reading, `fn` and writing. `fn` sees the record as stored (`exists` 1), or an
 * empty `pending_in` one for `id` (`exists` 0), and returns:
 *    0   store what it left in `st`
 *   >0   store nothing, and return this number from puddle_streams_update (its own meaning to the caller)
 *   <0   store nothing; update returns -1
 * `fn` must not call back into the store. Returns 0 when stored, otherwise as above, or -1 with `err` for an
 * invalid id, a blocked one, or a failed write. */
typedef int (*puddle_stream_update_fn)(void *user, puddle_stream *st, int exists);
int puddle_streams_update(puddle_streams *s, const char *id, puddle_stream_update_fn fn, void *user, char *err,
                          size_t errlen);

/* The provisional record with this ticket (a copy, whatever its state), or the one still waiting for a request to this address.
 * 0 found, 1 none. */
int puddle_streams_find_ticket(puddle_streams *s, const char *ticket, puddle_stream *out);
int puddle_streams_find_provisional(puddle_streams *s, const char *address, puddle_stream *out);

/* The answer to a provisional request says who it is: refile the record under `real_id` (state, note, addresses and
 * certificate carry over; it stops being provisional and its ticket is spent). If `real_id` already has a closed stream, it is
 * started afresh from the provisional one; any other existing record stands and the provisional one is dropped, as it is if
 * `real_id` is blocked. Returns 0 refiled, 1 dropped because a record existed,
 * 2 dropped because blocked, -1 on failure (nothing changed). */
int puddle_streams_adopt(puddle_streams *s, const char *provisional_id, const char *real_id, char *err, size_t errlen);

/* Forget a provisional record (the owner changed their mind). 0, 1 if there was none, -1. Only provisional ones. */
int puddle_streams_forget(puddle_streams *s, const char *provisional_id);

/* Every peer. Free with puddle_stream_list_free. 0 or -1. */
int puddle_streams_list(puddle_streams *s, puddle_stream **out, size_t *count);
void puddle_stream_list_free(puddle_stream *list, size_t count);

/* How many peers are in each state's `pending_in`, for limiting how many requests wait on the owner. */
size_t puddle_streams_count(puddle_streams *s, puddle_stream_state state);

/* How many peers are on record, in any state. */
size_t puddle_streams_size(puddle_streams *s);

/* The lookup request signatures need (it is a puddle_peer_key_fn): the current signing key of a peer we hold
 * a chain for, if `kid` names it. Retired keys, strangers, peers with no chain yet and blocked ids all say no.
 * `streams` is the puddle_streams. Which states may do what is the endpoints' business, not this one's. */
int puddle_streams_peer_key(void *streams, const char *sender_id, const char *kid, unsigned char sign_pub[32]);

/* Is `fingerprint_hex` the certificate we know for this peer? The first one seen is recorded and trusted (their
 * address can change; the certificate does not). 1 yes (or a peer we hold no record of, which is no one's to
 * judge), 0 it differs: it is remembered as `tls_fp_seen` for the owner to see, and nothing is sent to it.
 * The outbox calls this once the certificate is known and before anything is sent. */
int puddle_streams_check_fingerprint(puddle_streams *s, const char *id, const char *fingerprint_hex);
/* The owner trusts the certificate we met (`tls_fp_seen`): it becomes the known one. 0 done, 1 none was waiting, -1. */
int puddle_streams_trust_fingerprint(puddle_streams *s, const char *id, char *err, size_t errlen);

/* ---- blocks: by user id, local only, and they override everything ---- */

/* Block `id`. Closes any stream with it, and turns away its keys. 0 (also if already blocked) or -1. */
int puddle_streams_block(puddle_streams *s, const char *id, int64_t now_ms, char *err, size_t errlen);
int puddle_streams_unblock(puddle_streams *s, const char *id, char *err, size_t errlen); /* 0, 1 wasn't blocked, -1 */
int puddle_streams_is_blocked(puddle_streams *s, const char *id);
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    int64_t ts_ms;
} puddle_block;
/* Blocked ids with the time they were blocked, oldest first. Writes up to `max`; returns how many there are. */
size_t puddle_streams_blocked(puddle_streams *s, puddle_block *out, size_t max);

/* ---- what has been read ---- */

/* When the owner last read the chat with `id`, in our own clock's milliseconds (0 = never). Messages that arrived after it are unread. */
int64_t puddle_streams_read_marker(puddle_streams *s, const char *id);
/* Record that the chat with `id` has been read up to `ts_ms`. It only ever moves forward. 0 or -1. */
int puddle_streams_set_read_marker(puddle_streams *s, const char *id, int64_t ts_ms, char *err, size_t errlen);

/* ---- who may ask ---- */

typedef enum {
    PUDDLE_ACCESS_MANUAL, /* requests wait for the owner (the default) */
    PUDDLE_ACCESS_OPEN,   /* anyone is accepted at once */
    PUDDLE_ACCESS_CLOSED  /* strangers are ignored; a valid invite still works unless this is closed */
} puddle_access;

puddle_access puddle_streams_access(puddle_streams *s);
int puddle_streams_set_access(puddle_streams *s, puddle_access a, char *err, size_t errlen);
const char *puddle_access_name(puddle_access a);
int puddle_access_parse(const char *name, puddle_access *out);

/* ---- invites: tokens in a link that let someone in without waiting ---- */

typedef struct {
    char token[PUDDLE_INVITE_TOKEN_LEN + 1];
    int64_t created_ms, expires_ms;
    int uses; /* left */
} puddle_invite;

/* Make a token good for `uses` (1..1000) people and `ttl_ms` (up to 30 days). 0 or -1. */
int puddle_streams_invite_create(puddle_streams *s, int64_t now_ms, int64_t ttl_ms, int uses, puddle_invite *out,
                                 char *err, size_t errlen);
/* Spend one use of `token`. 1 if it was valid and has been spent, 0 if not (unknown, expired or used up).
 * The spend is saved first; if it can't be, the invite is not honoured. Every token is compared, without
 * stopping at the first match. */
int puddle_streams_invite_consume(puddle_streams *s, const char *token, int64_t now_ms);
/* Live invites. Writes up to `max`; returns how many there are. */
size_t puddle_streams_invites(puddle_streams *s, int64_t now_ms, puddle_invite *out, size_t max);
int puddle_streams_invite_revoke(puddle_streams *s, const char *token); /* 0, 1 no such token, -1 */

#ifdef __cplusplus
}
#endif
#endif
