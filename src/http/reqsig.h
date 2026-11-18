#ifndef PUDDLE_REQSIG_H
#define PUDDLE_REQSIG_H

/* Request signatures: how Alice's Puddle proves to Bob's that a request is from Alice, on every
 * request. Rules in docs/streams.md ("Request signatures"). The header is
 *
 *   X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>
 *
 * with `sig` an Ed25519 signature over: "puddle-request-v1", METHOD, request target, hex SHA-256 of the
 * body, ts, nonce and the recipient's user id, one per line. */

#include "core/delivery.h"
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

/* Something that signs: `user` is whatever holds the key. 0 or -1. */
typedef int (*puddle_sign_fn)(void *user, const void *data, size_t len, unsigned char sig[64]);

/* The same, for a signer that is not this Puddle's identity: a paired phone, with its device id as `signer_id`
 * (docs/identity.md, "Devices"). */
int puddle_reqsig_sign_as(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, const char *method,
                          const char *target, const void *body, size_t body_len, const char *recipient_id, int64_t now_ms, char *out,
                          size_t cap);

/* ---- sealed requests (the protocol's spec/09: bodies sealed to keys, so TLS is transport only) ----
 *
 *   X-Puddle-Seal: v1 kid=<hex SHA-256 of the recipient's sealing key>; enc=<base64 of HPKE's enc>
 *
 * The body is sealed (base/seal.h) to the recipient's key with info "puddle-seal-v1\n<sender>\n<recipient>\n<nonce>" and aad
 * "<METHOD>\n<target>", and the signature covers the sealed bytes. The answer comes back in records under `answer_key`. */

typedef struct {
    char signature[800];        /* X-Puddle-Signature */
    char seal[160];             /* X-Puddle-Seal */
    char nonce[32];
    unsigned char *body;        /* the sealed body, malloc'd; NULL when there was none */
    size_t body_len;
    unsigned char answer_key[32];
} puddle_sealed_request;

int puddle_seal_request(puddle_identity *me, const char *method, const char *target, const void *body, size_t body_len,
                        const char *recipient_id, const unsigned char seal_pub[32], int64_t now_ms, puddle_sealed_request *out);
int puddle_seal_request_as(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, const char *method,
                           const char *target, const void *body, size_t body_len, const char *recipient_id,
                           const unsigned char seal_pub[32], int64_t now_ms, puddle_sealed_request *out);
void puddle_sealed_request_free(puddle_sealed_request *r);

/* The HPKE info and aad both sides compute. The length, or 0 if it doesn't fit. */
size_t puddle_seal_info(const char *sender_id, const char *recipient_id, const char *nonce, char *out, size_t cap);
size_t puddle_seal_aad(const char *method, const char *target, char *out, size_t cap);

/* The recipient's side: parse X-Puddle-Seal. 0 or -1. */
int puddle_seal_header_parse(const char *header, char kid[65], unsigned char enc[PUDDLE_SEAL_ENC_LEN]);

#define PUDDLE_SEALED_TYPE "application/puddle-sealed"

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

/* ---- receipts: Bob's side signing for an answer, Alice's side checking it (spec/02 and spec/09 of the protocol) ----
 *
 *   X-Puddle-Receipt: v2 id=<responder id>; kid=<key id>; sig=<base64>
 *
 * `sig` is an Ed25519 signature over "puddle-receipt-v2", the responder's id, the request's nonce, the status and the hex
 * SHA-256 of the answer's body exactly as sent (sealed, when it is), one per line. No certificate: TLS is transport only. */

#define PUDDLE_RECEIPT_MAX 320 /* room for the header value */

/* Sign for the answer to the request with `nonce`. 0 or -1. */
int puddle_receipt_sign(puddle_identity *me, const char *nonce, int status, const void *body, size_t body_len, char *out, size_t cap);
/* The same, for a body that is not in memory (a file sent from disk): `body_hash_hex` is the SHA-256 of exactly the bytes sent. */
int puddle_receipt_sign_hash(puddle_identity *me, const char *nonce, int status, const char *body_hash_hex, char *out, size_t cap);

/* puddle_receipt_check is in core/delivery.h, beside the reply it describes */

/* Alice's side: is `header` (NULL or "" = none) the receipt `peer_id`, whose current key is `peer_pub`, gives for the
 * request with `nonce`, answered `status` with `body`? */
puddle_receipt_check puddle_receipt_verify(const char *header, const char *peer_id, const unsigned char peer_pub[32], const char *nonce,
                                           int status, const void *body, size_t body_len);
/* The same, for a body that was not kept in memory (a download written to a file): `body_hash_hex` is its SHA-256. */
puddle_receipt_check puddle_receipt_verify_hash(const char *header, const char *peer_id, const unsigned char peer_pub[32], const char *nonce,
                                                int status, const char *body_hash_hex);


#ifdef __cplusplus
}
#endif
#endif
