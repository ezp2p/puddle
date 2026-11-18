#include "base/seal.h"

#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

/* ------------------------------------------------------------ RFC 9180, section 4 */

/* the suite: DHKEM(X25519, HKDF-SHA256) = 0x0020, HKDF-SHA256 = 0x0001, ChaCha20-Poly1305 = 0x0003 */
static const unsigned char SUITE_KEM[] = { 'K', 'E', 'M', 0x00, 0x20 };
static const unsigned char SUITE_HPKE[] = { 'H', 'P', 'K', 'E', 0x00, 0x20, 0x00, 0x01, 0x00, 0x03 };

struct Bytes { /* a bounded buffer; anything that would overflow it marks it bad */
    unsigned char b[512];
    size_t n = 0;
    bool bad = false;
    void add(const void *p, size_t len)
    {
        if (bad || len > sizeof b - n) {
            bad = true;
            return;
        }
        if (len)
            memcpy(b + n, p, len);
        n += len;
    }
    void add(const char *s) { add(s, strlen(s)); }
};

/* HKDF-Extract: HMAC-SHA256 keyed by the salt (32 zero bytes when there is none) */
static bool extract(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len, unsigned char prk[32])
{
    static const unsigned char zeros[32] = { 0 };
    unsigned len = 0;
    return HMAC(EVP_sha256(), salt_len ? salt : zeros, salt_len ? (int)salt_len : 32, (const unsigned char *)ikm, ikm_len, prk, &len) &&
           len == 32;
}

/* HKDF-Expand to `out_len` bytes (at most 255 * 32) */
static bool expand(const unsigned char prk[32], const void *info, size_t info_len, unsigned char *out, size_t out_len)
{
    unsigned char t[32];
    size_t done = 0, tlen = 0;
    for (unsigned char i = 1; done < out_len; i++) {
        Bytes in;
        in.add(t, tlen);
        in.add(info, info_len);
        in.add(&i, 1);
        unsigned len = 0;
        if (in.bad || !HMAC(EVP_sha256(), prk, 32, in.b, in.n, t, &len) || len != 32)
            return false;
        tlen = 32;
        size_t take = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, t, take);
        done += take;
    }
    OPENSSL_cleanse(t, sizeof t);
    return true;
}

static bool labeled_extract(const unsigned char *suite, size_t suite_len, const void *salt, size_t salt_len, const char *label,
                            const void *ikm, size_t ikm_len, unsigned char prk[32])
{
    Bytes in;
    in.add("HPKE-v1");
    in.add(suite, suite_len);
    in.add(label);
    in.add(ikm, ikm_len);
    bool ok = !in.bad && extract(salt, salt_len, in.b, in.n, prk);
    OPENSSL_cleanse(in.b, in.n);
    return ok;
}

static bool labeled_expand(const unsigned char *suite, size_t suite_len, const unsigned char prk[32], const char *label,
                           const void *info, size_t info_len, unsigned char *out, size_t out_len)
{
    if (out_len > 0xffff)
        return false;
    Bytes in;
    unsigned char l[2] = { (unsigned char)(out_len >> 8), (unsigned char)out_len };
    in.add(l, 2);
    in.add("HPKE-v1");
    in.add(suite, suite_len);
    in.add(label);
    in.add(info, info_len);
    return !in.bad && expand(prk, in.b, in.n, out, out_len);
}

/* ------------------------------------------------------------ X25519 */

int puddle_x25519_public(const unsigned char sk[32], unsigned char pk[32])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, sk, 32);
    size_t len = 32;
    int ok = k && EVP_PKEY_get_raw_public_key(k, pk, &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    return ok ? 0 : -1;
}

int puddle_x25519_derive(const void *ikm, size_t ikm_len, unsigned char sk[32], unsigned char pk[32])
{
    unsigned char prk[32];
    bool ok = labeled_extract(SUITE_KEM, sizeof SUITE_KEM, NULL, 0, "dkp_prk", ikm, ikm_len, prk) &&
              labeled_expand(SUITE_KEM, sizeof SUITE_KEM, prk, "sk", NULL, 0, sk, 32) && puddle_x25519_public(sk, pk) == 0;
    OPENSSL_cleanse(prk, sizeof prk);
    return ok ? 0 : -1;
}

static bool dh(const unsigned char sk[32], const unsigned char pk[32], unsigned char out[32])
{
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, sk, 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pk, 32);
    EVP_PKEY_CTX *c = mine ? EVP_PKEY_CTX_new(mine, NULL) : NULL;
    size_t len = 32;
    bool ok = c && theirs && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, theirs) == 1 &&
              EVP_PKEY_derive(c, out, &len) == 1 && len == 32;
    if (ok) { /* an all-zero result means a key of low order: refuse it (RFC 9180, 7.1.4) */
        unsigned char any = 0;
        for (int i = 0; i < 32; i++)
            any |= out[i];
        ok = any != 0;
    }
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(theirs);
    return ok;
}

/* ------------------------------------------------------------ the KEM and the key schedule */

static bool shared_secret(const unsigned char dh_out[32], const unsigned char enc[32], const unsigned char pk_r[32], unsigned char out[32])
{
    unsigned char prk[32], context[64];
    memcpy(context, enc, 32);
    memcpy(context + 32, pk_r, 32);
    bool ok = labeled_extract(SUITE_KEM, sizeof SUITE_KEM, NULL, 0, "eae_prk", dh_out, 32, prk) &&
              labeled_expand(SUITE_KEM, sizeof SUITE_KEM, prk, "shared_secret", context, 64, out, 32);
    OPENSSL_cleanse(prk, sizeof prk);
    return ok;
}

static bool key_schedule(const unsigned char shared[32], const void *info, size_t info_len, puddle_hpke *ctx)
{
    unsigned char ksc[65], secret[32];
    ksc[0] = 0; /* mode_base */
    bool ok = labeled_extract(SUITE_HPKE, sizeof SUITE_HPKE, NULL, 0, "psk_id_hash", NULL, 0, ksc + 1) &&
              labeled_extract(SUITE_HPKE, sizeof SUITE_HPKE, NULL, 0, "info_hash", info, info_len, ksc + 33) &&
              labeled_extract(SUITE_HPKE, sizeof SUITE_HPKE, shared, 32, "secret", NULL, 0, secret) &&
              labeled_expand(SUITE_HPKE, sizeof SUITE_HPKE, secret, "key", ksc, sizeof ksc, ctx->key, 32) &&
              labeled_expand(SUITE_HPKE, sizeof SUITE_HPKE, secret, "base_nonce", ksc, sizeof ksc, ctx->base_nonce, 12) &&
              labeled_expand(SUITE_HPKE, sizeof SUITE_HPKE, secret, "exp", ksc, sizeof ksc, ctx->exporter, 32);
    ctx->seq = 0;
    OPENSSL_cleanse(secret, sizeof secret);
    if (!ok)
        OPENSSL_cleanse(ctx, sizeof *ctx);
    return ok;
}

int puddle_hpke_sender_with(const unsigned char sk_e[32], const unsigned char pk_r[32], const void *info, size_t info_len,
                            unsigned char enc[32], puddle_hpke *ctx)
{
    unsigned char d[32], shared[32];
    bool ok = puddle_x25519_public(sk_e, enc) == 0 && dh(sk_e, pk_r, d) && shared_secret(d, enc, pk_r, shared) &&
              key_schedule(shared, info, info_len, ctx);
    OPENSSL_cleanse(d, sizeof d);
    OPENSSL_cleanse(shared, sizeof shared);
    return ok ? 0 : -1;
}

int puddle_hpke_sender(const unsigned char pk_r[32], const void *info, size_t info_len, unsigned char enc[32], puddle_hpke *ctx)
{
    unsigned char ikm[32], sk[32], pk[32];
    bool ok = RAND_bytes(ikm, sizeof ikm) == 1 && puddle_x25519_derive(ikm, sizeof ikm, sk, pk) == 0 &&
              puddle_hpke_sender_with(sk, pk_r, info, info_len, enc, ctx) == 0;
    OPENSSL_cleanse(ikm, sizeof ikm);
    OPENSSL_cleanse(sk, sizeof sk);
    return ok ? 0 : -1;
}

int puddle_hpke_receiver(const unsigned char enc[32], const unsigned char sk_r[32], const void *info, size_t info_len, puddle_hpke *ctx)
{
    unsigned char pk_r[32], d[32], shared[32];
    bool ok = puddle_x25519_public(sk_r, pk_r) == 0 && dh(sk_r, enc, d) && shared_secret(d, enc, pk_r, shared) &&
              key_schedule(shared, info, info_len, ctx);
    OPENSSL_cleanse(d, sizeof d);
    OPENSSL_cleanse(shared, sizeof shared);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------ ChaCha20-Poly1305 */

static bool aead(bool seal, const unsigned char key[32], const unsigned char nonce[12], const void *aad, size_t aad_len,
                 const unsigned char *in, size_t len, unsigned char *out)
{
    if (!seal && len < PUDDLE_SEAL_TAG_LEN)
        return false;
    size_t body = seal ? len : len - PUDDLE_SEAL_TAG_LEN;
    if (body > 0x7fffffff || aad_len > 0x7fffffff)
        return false;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0;
    bool ok = c && (seal ? EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, nonce)
                         : EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, nonce)) == 1;
    if (ok && aad_len)
        ok = (seal ? EVP_EncryptUpdate(c, NULL, &n, (const unsigned char *)aad, (int)aad_len)
                   : EVP_DecryptUpdate(c, NULL, &n, (const unsigned char *)aad, (int)aad_len)) == 1;
    if (ok && body)
        ok = (seal ? EVP_EncryptUpdate(c, out, &n, in, (int)body) : EVP_DecryptUpdate(c, out, &n, in, (int)body)) == 1;
    if (ok && seal)
        ok = EVP_EncryptFinal_ex(c, out + body, &n) == 1 &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, PUDDLE_SEAL_TAG_LEN, out + body) == 1;
    else if (ok)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, PUDDLE_SEAL_TAG_LEN, (void *)(in + body)) == 1 &&
             EVP_DecryptFinal_ex(c, out + body, &n) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok && !seal && body)
        OPENSSL_cleanse(out, body); /* nothing of a body that failed its tag is left behind */
    return ok;
}

static void nonce_for(const unsigned char base[12], uint64_t seq, unsigned char out[12])
{
    memcpy(out, base, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (unsigned char)(seq >> (8 * i));
}

int puddle_hpke_seal(puddle_hpke *ctx, const void *aad, size_t aad_len, const void *pt, size_t len, unsigned char *out)
{
    unsigned char nonce[12];
    nonce_for(ctx->base_nonce, ctx->seq, nonce);
    if (ctx->seq == UINT64_MAX || !aead(true, ctx->key, nonce, aad, aad_len, (const unsigned char *)pt, len, out))
        return -1;
    ctx->seq++;
    return 0;
}

int puddle_hpke_open(puddle_hpke *ctx, const void *aad, size_t aad_len, const unsigned char *ct, size_t len, unsigned char *out)
{
    unsigned char nonce[12];
    nonce_for(ctx->base_nonce, ctx->seq, nonce);
    if (ctx->seq == UINT64_MAX || !aead(false, ctx->key, nonce, aad, aad_len, ct, len, out))
        return -1;
    ctx->seq++;
    return 0;
}

int puddle_hpke_export(const puddle_hpke *ctx, const void *label, size_t label_len, unsigned char *out, size_t out_len)
{
    return labeled_expand(SUITE_HPKE, sizeof SUITE_HPKE, ctx->exporter, "sec", label, label_len, out, out_len) ? 0 : -1;
}

/* ------------------------------------------------------------ answers */

#define ANSWER_LABEL "puddle-answer-v1"

int puddle_seal_answer_key(const puddle_hpke *ctx, unsigned char key[32])
{
    return puddle_hpke_export(ctx, ANSWER_LABEL, strlen(ANSWER_LABEL), key, 32);
}

static size_t records_for(size_t plain_len)
{
    return plain_len == 0 ? 1 : (plain_len + PUDDLE_SEAL_RECORD - 1) / PUDDLE_SEAL_RECORD;
}

size_t puddle_seal_answer_len(size_t plain_len)
{
    return plain_len + records_for(plain_len) * PUDDLE_SEAL_TAG_LEN;
}

size_t puddle_seal_plain_len(size_t sealed_len)
{
    const size_t full = PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN;
    size_t whole = sealed_len / full, rest = sealed_len % full;
    if (rest == 0)
        return whole ? sealed_len - whole * PUDDLE_SEAL_TAG_LEN : 0;
    if (rest < PUDDLE_SEAL_TAG_LEN)
        return 0;
    return sealed_len - (whole + 1) * PUDDLE_SEAL_TAG_LEN;
}

static size_t record_aad(int status, int last, char out[40])
{
    return (size_t)snprintf(out, 40, ANSWER_LABEL "\n%d\n%d", status, last ? 1 : 0);
}

static void record_nonce(uint64_t index, unsigned char nonce[12])
{
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] = (unsigned char)(index >> (8 * i));
}

int puddle_seal_record(const unsigned char key[32], uint64_t index, int status, int last, const void *pt, size_t len,
                       unsigned char *out)
{
    char aad[40];
    unsigned char nonce[12];
    record_nonce(index, nonce);
    return len <= PUDDLE_SEAL_RECORD && aead(true, key, nonce, aad, record_aad(status, last, aad), (const unsigned char *)pt, len, out)
               ? 0
               : -1;
}

int puddle_open_record(const unsigned char key[32], uint64_t index, int status, int last, const unsigned char *ct, size_t len,
                       unsigned char *out)
{
    char aad[40];
    unsigned char nonce[12];
    record_nonce(index, nonce);
    return len <= PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN && aead(false, key, nonce, aad, record_aad(status, last, aad), ct, len, out)
               ? 0
               : -1;
}

int puddle_seal_answer(const unsigned char key[32], int status, const void *pt, size_t len, unsigned char *out)
{
    size_t n = records_for(len);
    const unsigned char *p = (const unsigned char *)pt;
    for (size_t i = 0; i < n; i++) {
        size_t take = i + 1 < n ? PUDDLE_SEAL_RECORD : len - i * PUDDLE_SEAL_RECORD;
        if (puddle_seal_record(key, i, status, i + 1 == n, p + i * PUDDLE_SEAL_RECORD, take,
                               out + i * (PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN)) != 0)
            return -1;
    }
    return 0;
}

int puddle_open_answer(const unsigned char key[32], int status, const unsigned char *ct, size_t len, unsigned char *out, size_t *out_len)
{
    const size_t full = PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN;
    size_t plain = puddle_seal_plain_len(len);
    if (len < PUDDLE_SEAL_TAG_LEN || (plain == 0 && len != PUDDLE_SEAL_TAG_LEN))
        return -1;
    size_t n = records_for(plain);
    for (size_t i = 0; i < n; i++) {
        size_t take = i + 1 < n ? full : len - i * full;
        if (puddle_open_record(key, i, status, i + 1 == n, ct + i * full, take, out + i * PUDDLE_SEAL_RECORD) != 0)
            return -1;
    }
    *out_len = plain;
    return 0;
}
