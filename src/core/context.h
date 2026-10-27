#ifndef PUDDLE_CONTEXT_H
#define PUDDLE_CONTEXT_H

/* What a running Puddle is made of, handed to everything that acts for it: the owner's routes and objects, the
 * routes other Puddles call, and the outbox's callbacks. service/server.cpp builds it. Each part is named here
 * without saying what it is, so this header needs nothing from the layers above core. */

#include "core/config.h"
#include "core/link.h"
#include "core/profile.h"
#include "core/streams.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puddle_identity puddle_identity;
typedef struct puddle_store puddle_store;
typedef struct puddle_streams puddle_streams;
typedef struct puddle_people puddle_people;
typedef struct puddle_conversations puddle_conversations;
typedef struct puddle_groups puddle_groups;
typedef struct puddle_outbox puddle_outbox;
typedef struct puddle_replay puddle_replay;
typedef struct puddle_bucket puddle_bucket;
typedef struct puddle_keyed puddle_keyed;
typedef int (*puddle_peer_key_fn)(void *user, const char *sender_id, const char *kid, unsigned char sign_pub[32]);

/* A roll of the keys that has been started and is waiting for the owner to say the new phrase is written down. */
typedef struct puddle_roll_slot puddle_roll_slot; /* api/api.h makes and frees one */

typedef struct puddle_ctx puddle_ctx;

/* What the owner does that other Puddles have to hear about: asking for a stream, answering one, a group's news,
 * a profile change, keys that rolled. federation/ carries each out (federation/courier.h); the owner's objects and
 * routes call it through the context, so they never depend on how Puddles talk to each other. Each is the
 * federation function of the same name: see there for what it returns. */
typedef struct {
    int (*request)(puddle_ctx *ctx, const puddle_link *link, const char *message, const char *profile_id,
                   char out_id[PUDDLE_USER_ID_LEN + 1], const char **code, const char **message_out);
    int (*owe)(puddle_ctx *ctx, const char *peer, puddle_owed owed);
    int (*profile_push)(puddle_ctx *ctx, const char *place);         /* the choice for a place changed: tell whoever is there */
    int (*profile_changed)(puddle_ctx *ctx, const char *profile_id); /* a profile was edited: tell every place showing it */
    int (*media_want)(puddle_ctx *ctx, const char *peer);
    int (*keys_rolled)(puddle_ctx *ctx);
    int (*group_create)(puddle_ctx *ctx, const char *name, char gid[PUDDLE_USER_ID_LEN + 1], const char **code);
    int (*group_invite)(puddle_ctx *ctx, const char *gid, const char *invitee, const char **code);
    int (*group_accept)(puddle_ctx *ctx, const char *gid, const char **code);
    int (*group_decline)(puddle_ctx *ctx, const char *gid, const char **code);
    int (*group_leave)(puddle_ctx *ctx, const char *gid, const char **code);
    int (*group_rename)(puddle_ctx *ctx, const char *gid, const char *name, const char **code);
    int (*group_send_event)(puddle_ctx *ctx, const char *gid, const char *event, size_t len, char id[65], int *sent, const char **code);
} puddle_courier;

/* What handlers can reach. */
struct puddle_ctx {
    const puddle_config *config;
    puddle_identity *identity;
    puddle_store *store;
    puddle_profiles *profiles;              /* the owner's profiles (core/profile.h) */
    puddle_assignments *assignments;
    puddle_streams *streams;
    puddle_people *people;                 /* the profiles other Puddles sent us; NULL in tests that keep none */
    puddle_conversations *conversations;   /* conversation summaries; NULL = work each out from its log every time */
    puddle_groups *groups;                 /* the group chats we are in or invited to; NULL in tests that have none */
    puddle_outbox *outbox;                 /* what we owe other Puddles; NULL in tests that do not send */
    puddle_replay *replay;                 /* one nonce memory for the whole Puddle */
    puddle_bucket *anon_bucket;            /* work done for strangers before they are authenticated; NULL = unlimited */
    puddle_keyed *access_limit;            /* requests for a stream, per requester; NULL = unlimited */
    char own_tls_fp[65];                   /* the SHA-256 of our certificate, hex; "" if not known */
    size_t max_peers;                      /* records on file before new strangers are ignored; 0 = PUDDLE_MAX_PEERS */
    puddle_roll_slot *roll_slot;
    puddle_peer_key_fn peer_key;           /* PEER routes: current signing key of a known peer (puddle_streams_peer_key); NULL = none known */
    void *peer_key_user;
    int64_t (*now_ms)(void);               /* NULL = the clock; a seam for tests */
    const puddle_courier *courier;         /* how the owner's news reaches other Puddles (federation/courier.h) */
};

/* The time in milliseconds, from the context's clock when there is one (tests set it). */
int64_t puddle_ctx_now_ms(const puddle_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif
