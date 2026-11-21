/* Puddle's face on libpuddle-proto's request signatures, receipts and sealed requests: the same calls, signing with this
 * Puddle's identity (or a phone's key), on Puddle's clock. */

#include "http/reqsig.h"

#include "platform/clock.h"

#include <string.h>

static int identity_sign(void *user, const void *data, size_t len, unsigned char sig[64])
{
    return puddle_identity_sign((puddle_identity *)user, data, len, sig);
}

/* A signer for this Puddle's identity; `id` is filled and must outlive it. */
static void identity_signer(puddle_identity *me, char id[PUDDLE_USER_ID_LEN + 1], pp_signer *s)
{
    memset(s, 0, sizeof *s);
    puddle_identity_id(me, id);
    puddle_identity_public(me, s->pub, NULL, NULL);
    s->id = id;
    s->sign = identity_sign;
    s->user = me;
}

static void other_signer(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, pp_signer *s)
{
    memset(s, 0, sizeof *s);
    s->id = signer_id;
    if (sign_pub)
        memcpy(s->pub, sign_pub, 32);
    s->sign = sign;
    s->user = user;
}

/* Puddle's clock (platform/clock.h); the library's own is the wall clock too, but this is the one Puddle's tests know. */
static int64_t at_or_now(int64_t at_ms)
{
    return at_ms ? at_ms : puddle_wall_ms();
}

int puddle_reqsig_sign(puddle_identity *me, const char *method, const char *target, const void *body, size_t body_len,
                       const char *recipient_id, int64_t at_ms, char *out, size_t cap)
{
    if (!me)
        return -1;
    char id[PUDDLE_USER_ID_LEN + 1];
    pp_signer s;
    identity_signer(me, id, &s);
    return pp_reqsig_sign(&s, method, target, body, body_len, recipient_id, at_or_now(at_ms), NULL, out, cap);
}

int puddle_reqsig_sign_as(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, const char *method,
                          const char *target, const void *body, size_t body_len, const char *recipient_id, int64_t at_ms, char *out,
                          size_t cap)
{
    pp_signer s;
    other_signer(signer_id, sign_pub, sign, user, &s);
    return pp_reqsig_sign(&s, method, target, body, body_len, recipient_id, at_or_now(at_ms), NULL, out, cap);
}

int puddle_reqsig_parse(const char *header, puddle_reqsig *out)
{
    return pp_reqsig_parse(header, out);
}

puddle_replay *puddle_replay_new(size_t capacity)
{
    return pp_replay_new(capacity);
}

void puddle_replay_free(puddle_replay *r)
{
    pp_replay_free(r);
}

puddle_authn puddle_reqsig_authenticate(const char *header, const char *method, const char *target, const void *body, size_t body_len,
                                        const char *my_id, puddle_peer_key_fn lookup, void *user, puddle_replay *replay, int64_t at_ms,
                                        puddle_reqsig *out)
{
    return pp_reqsig_authenticate(header, method, target, body, body_len, my_id, lookup, user, replay, at_or_now(at_ms), out);
}

/* ------------------------------------------------------------ receipts */

static puddle_receipt_check from_pp(pp_receipt_check c)
{
    switch (c) {
    case PP_RECEIPT_OK: return PUDDLE_RECEIPT_OK;
    case PP_RECEIPT_MISSING: return PUDDLE_RECEIPT_MISSING;
    case PP_RECEIPT_WRONG_KEY: return PUDDLE_RECEIPT_WRONG_KEY;
    default: return PUDDLE_RECEIPT_BAD;
    }
}

int puddle_receipt_sign(puddle_identity *me, const char *nonce, int status, const void *body, size_t body_len, char *out, size_t cap)
{
    char hash[65];
    pp_sha256_hex(body ? body : "", body ? body_len : 0, hash);
    return puddle_receipt_sign_hash(me, nonce, status, hash, out, cap);
}

int puddle_receipt_sign_hash(puddle_identity *me, const char *nonce, int status, const char *body_hash_hex, char *out, size_t cap)
{
    if (!me)
        return -1;
    char id[PUDDLE_USER_ID_LEN + 1];
    pp_signer s;
    identity_signer(me, id, &s);
    return pp_receipt_sign(&s, nonce, status, body_hash_hex, out, cap);
}

puddle_receipt_check puddle_receipt_verify(const char *header, const char *peer_id, const unsigned char peer_pub[32], const char *nonce,
                                           int status, const void *body, size_t body_len)
{
    char hash[65];
    pp_sha256_hex(body ? body : "", body ? body_len : 0, hash);
    return puddle_receipt_verify_hash(header, peer_id, peer_pub, nonce, status, hash);
}

puddle_receipt_check puddle_receipt_verify_hash(const char *header, const char *peer_id, const unsigned char peer_pub[32], const char *nonce,
                                                int status, const char *body_hash_hex)
{
    return from_pp(pp_receipt_verify(header, peer_id, peer_pub, nonce, status, body_hash_hex));
}

/* ------------------------------------------------------------ sealed requests (spec/09) */

size_t puddle_seal_info(const char *sender_id, const char *recipient_id, const char *nonce, char *out, size_t cap)
{
    return pp_seal_info(sender_id, recipient_id, nonce, out, cap);
}

size_t puddle_seal_aad(const char *method, const char *target, char *out, size_t cap)
{
    return pp_seal_aad(method, target, out, cap);
}

void puddle_sealed_request_free(puddle_sealed_request *r)
{
    pp_sealed_request_free(r);
}

int puddle_seal_request_as(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, const char *method,
                           const char *target, const void *body, size_t body_len, const char *recipient_id,
                           const unsigned char seal_pub[32], int64_t at_ms, puddle_sealed_request *out)
{
    pp_signer s;
    other_signer(signer_id, sign_pub, sign, user, &s);
    return pp_seal_request(&s, method, target, body, body_len, recipient_id, seal_pub, at_or_now(at_ms), out);
}

int puddle_seal_request(puddle_identity *me, const char *method, const char *target, const void *body, size_t body_len,
                        const char *recipient_id, const unsigned char seal_pub[32], int64_t at_ms, puddle_sealed_request *out)
{
    if (!me) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    char id[PUDDLE_USER_ID_LEN + 1];
    pp_signer s;
    identity_signer(me, id, &s);
    return pp_seal_request(&s, method, target, body, body_len, recipient_id, seal_pub, at_or_now(at_ms), out);
}

int puddle_seal_header_parse(const char *header, char kid[65], unsigned char enc[PUDDLE_SEAL_ENC_LEN])
{
    return pp_seal_header_parse(header, kid, enc);
}
