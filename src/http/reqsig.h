#ifndef PUDDLE_REQSIG_H
#define PUDDLE_REQSIG_H

/* Request signatures: how Alice's Puddle proves to Bob's that a request is from Alice, on every
 * request. Rules in docs/streams.md ("Request signatures"). The header is
 *
 *   X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>
 *
 * with `sig` an Ed25519 signature over: "puddle-request-v1", METHOD, request target, hex SHA-256 of the
 * body, ts, nonce and the recipient's user id, one per line. */

#include "core/identity.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_SIG_WINDOW_MS (5 * 60 * 1000)

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    char kid[65];
    int64_t ts;
    char nonce[48];
    unsigned char sig[64];
} puddle_reqsig;

/* The recipient to sign for when the sender does not know who lives at an address yet (a first request, see streams.md). Only a
 * route that says it takes such requests accepts one; everywhere else it is the usual 404. */
#define PUDDLE_RECIPIENT_ANY "*"

/* Alice's side: build the header value for a request to `recipient_id`. `now_ms` 0 = now.
 * 0 or -1 (bad input, or `cap` too small; 400 bytes is plenty). */
int puddle_reqsig_sign(puddle_identity *me, const char *method, const char *target, const void *body,
                       size_t body_len, const char *recipient_id, int64_t now_ms, char *out, size_t cap);

/* Strictly parse a header value: exact field order, exact formats, nothing extra. 0 or -1. */
int puddle_reqsig_parse(const char *header, puddle_reqsig *out);

/* Remembers nonces for ten minutes so a captured request can't be replayed. */
typedef struct puddle_replay puddle_replay;
puddle_replay *puddle_replay_new(size_t capacity); /* a power of two >= 8; 0 = 65536 */
void puddle_replay_free(puddle_replay *r);

typedef enum {
    PUDDLE_AUTHN_OK = 0,
    PUDDLE_AUTHN_REJECT = 1,   /* not authenticated: answer as if the path did not exist */
    PUDDLE_AUTHN_SKEW = 2,     /* authenticated, but ts is off by more than five minutes */
    PUDDLE_AUTHN_REPLAYED = 3, /* authenticated, but this exact request was already seen */
    PUDDLE_AUTHN_BUSY = 4      /* authenticated, but the nonce memory is full */
} puddle_authn;

/* Look up the sender's *current* signing key. `kid` is the key id the request names. Return 1 and fill
 * `sign_pub` only if `sender_id` is someone we know and `kid` is their current key; a retired key must
 * return 0. For a stream request from a stranger, return the key from the chain in the body. */
typedef int (*puddle_peer_key_fn)(void *user, const char *sender_id, const char *kid, unsigned char sign_pub[32]);

/* Bob's side: decide whether a request really is from who it says. In this order, stopping at the
 * first failure: parse; sender and key known; signature (which covers `my_id`, so a request meant for
 * another Puddle fails here); clock; replay. Everything up to the signature is REJECT, so a stranger
 * learns nothing. The nonce is remembered only after the signature has passed. `out` may be NULL. */
puddle_authn puddle_reqsig_authenticate(const char *header, const char *method, const char *target,
                                        const void *body, size_t body_len, const char *my_id,
                                        puddle_peer_key_fn lookup, void *user, puddle_replay *replay,
                                        int64_t now_ms, puddle_reqsig *out);

#ifdef __cplusplus
}
#endif
#endif
