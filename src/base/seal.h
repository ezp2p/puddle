#ifndef PUDDLE_SEAL_H
#define PUDDLE_SEAL_H

/* Sealing (the Puddle Protocol, spec/09-sealing.md): what keeps bodies private whatever carries them, so TLS is
 * transport only.
 *
 * HPKE (RFC 9180), base mode, one suite: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20-Poly1305. Built here over
 * OpenSSL 3.0's primitives, since OpenSSL's own HPKE needs 3.2; checked against RFC 9180's vectors (seal_test).
 *
 * A request body is sealed to the recipient's key with an HPKE context; the answer comes back in records under a key
 * exported from that same context, so only the caller and the recipient can read either. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_SEAL_KEY_LEN 32   /* an X25519 key, private or public */
#define PUDDLE_SEAL_ENC_LEN 32   /* HPKE's enc: the sender's one-time public key */
#define PUDDLE_SEAL_TAG_LEN 16   /* what sealing adds to each piece */
#define PUDDLE_SEAL_RECORD 65536 /* plaintext in each answer record but the last */

typedef struct {
    unsigned char key[32];
    unsigned char base_nonce[12];
    unsigned char exporter[32];
    uint64_t seq;
} puddle_hpke;

/* RFC 9180 DeriveKeyPair: an X25519 key pair from `ikm` (at least 32 bytes of secret). 0 or -1. */
int puddle_x25519_derive(const void *ikm, size_t ikm_len, unsigned char sk[32], unsigned char pk[32]);

/* The public key of an X25519 private key. 0 or -1. */
int puddle_x25519_public(const unsigned char sk[32], unsigned char pk[32]);

/* A sender's context to `pk_r`, with a fresh one-time key; `enc` is what the recipient needs to open it. 0 or -1.
 * puddle_hpke_sender_with takes the one-time private key instead (tests, against RFC 9180's vectors). */
int puddle_hpke_sender(const unsigned char pk_r[32], const void *info, size_t info_len, unsigned char enc[32], puddle_hpke *ctx);
int puddle_hpke_sender_with(const unsigned char sk_e[32], const unsigned char pk_r[32], const void *info, size_t info_len,
                            unsigned char enc[32], puddle_hpke *ctx);

/* The recipient's context from `enc`, with its private key. 0 or -1. */
int puddle_hpke_receiver(const unsigned char enc[32], const unsigned char sk_r[32], const void *info, size_t info_len, puddle_hpke *ctx);

/* Seal `len` bytes into `out` (len + PUDDLE_SEAL_TAG_LEN bytes), or open `len` bytes into `out` (len - TAG bytes). Each
 * call uses the context's next sequence number. 0, or -1 (for open: it was not sealed for this context). */
int puddle_hpke_seal(puddle_hpke *ctx, const void *aad, size_t aad_len, const void *pt, size_t len, unsigned char *out);
int puddle_hpke_open(puddle_hpke *ctx, const void *aad, size_t aad_len, const unsigned char *ct, size_t len, unsigned char *out);

/* HPKE Export: `out_len` bytes of secret bound to this context and `label`. 0 or -1. */
int puddle_hpke_export(const puddle_hpke *ctx, const void *label, size_t label_len, unsigned char *out, size_t out_len);

/* ---- answers: records under a key exported from the request's context ("puddle-answer-v1") ---- */

/* The answer key for a request's context. 0 or -1. */
int puddle_seal_answer_key(const puddle_hpke *ctx, unsigned char key[32]);

/* How many bytes an answer of `plain_len` bytes is once sealed, and the most plaintext `sealed_len` bytes can hold
 * (0 if they can't be a sealed answer). */
size_t puddle_seal_answer_len(size_t plain_len);
size_t puddle_seal_plain_len(size_t sealed_len);

/* One record: `index` from 0, `last` 1 for the final one. Seal writes len + TAG bytes; open writes len - TAG. 0 or -1. */
int puddle_seal_record(const unsigned char key[32], uint64_t index, int status, int last, const void *pt, size_t len,
                       unsigned char *out);
int puddle_open_record(const unsigned char key[32], uint64_t index, int status, int last, const unsigned char *ct, size_t len,
                       unsigned char *out);

/* A whole answer at once: seal `len` bytes into `out` (puddle_seal_answer_len(len) bytes), or open `len` sealed bytes into
 * `out` (puddle_seal_plain_len(len) bytes), saying in *out_len how many. 0 or -1. */
int puddle_seal_answer(const unsigned char key[32], int status, const void *pt, size_t len, unsigned char *out);
int puddle_open_answer(const unsigned char key[32], int status, const unsigned char *ct, size_t len, unsigned char *out, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif
