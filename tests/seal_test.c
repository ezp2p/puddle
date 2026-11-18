/* Sealing (base/seal.h): HPKE checked against RFC 9180's own vectors (A.2.1: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256,
 * ChaCha20-Poly1305, base mode), then answers in records. */
#include "base/seal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static size_t unhex(const char *s, unsigned char *out)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

static int same_hex(const unsigned char *b, size_t len, const char *hex)
{
    unsigned char want[256];
    return unhex(hex, want) == len && memcmp(b, want, len) == 0;
}

static void test_rfc9180(void)
{
    unsigned char info[64], ikm_e[32], ikm_r[32], sk_e[32], pk_e[32], sk_r[32], pk_r[32], enc[32];
    size_t info_len = unhex("4f6465206f6e2061204772656369616e2055726e", info);
    unhex("909a9b35d3dc4713a5e72a4da274b55d3d3821a37e5d099e74a647db583a904b", ikm_e);
    unhex("1ac01f181fdf9f352797655161c58b75c656a6cc2716dcb66372da835542e1df", ikm_r);

    CHECK(puddle_x25519_derive(ikm_e, 32, sk_e, pk_e) == 0);
    CHECK(same_hex(sk_e, 32, "f4ec9b33b792c372c1d2c2063507b684ef925b8c75a42dbcbf57d63ccd381600"));
    CHECK(same_hex(pk_e, 32, "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a"));
    CHECK(puddle_x25519_derive(ikm_r, 32, sk_r, pk_r) == 0);
    CHECK(same_hex(sk_r, 32, "8057991eef8f1f1af18f4a9491d16a1ce333f695d4db8e38da75975c4478e0fb"));
    CHECK(same_hex(pk_r, 32, "4310ee97d88cc1f088a5576c77ab0cf5c3ac797f3d95139c6c84b5429c59662a"));

    puddle_hpke s, r;
    CHECK(puddle_hpke_sender_with(sk_e, pk_r, info, info_len, enc, &s) == 0);
    CHECK(same_hex(enc, 32, "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a"));
    CHECK(same_hex(s.key, 32, "ad2744de8e17f4ebba575b3f5f5a8fa1f69c2a07f6e7500bc60ca6e3e3ec1c91"));
    CHECK(same_hex(s.base_nonce, 12, "5c4d98150661b848853b547f"));
    CHECK(same_hex(s.exporter, 32, "a3b010d4994890e2c6968a36f64470d3c824c8f5029942feb11e7a74b2921922"));
    CHECK(puddle_hpke_receiver(enc, sk_r, info, info_len, &r) == 0);
    CHECK(memcmp(s.key, r.key, 32) == 0 && memcmp(s.base_nonce, r.base_nonce, 12) == 0 && memcmp(s.exporter, r.exporter, 32) == 0);

    /* encryptions: sequence numbers 0, 1, 2, 4, 255, 256 */
    static const struct { unsigned seq; const char *aad, *ct; } enc_vectors[] = {
        { 0, "436f756e742d30", "1c5250d8034ec2b784ba2cfd69dbdb8af406cfe3ff938e131f0def8c8b60b4db21993c62ce81883d2dd1b51a28" },
        { 1, "436f756e742d31", "6b53c051e4199c518de79594e1c4ab18b96f081549d45ce015be002090bb119e85285337cc95ba5f59992dc98c" },
        { 2, "436f756e742d32", "71146bd6795ccc9c49ce25dda112a48f202ad220559502cef1f34271e0cb4b02b4f10ecac6f48c32f878fae86b" },
        { 4, "436f756e742d34", "63357a2aa291f5a4e5f27db6baa2af8cf77427c7c1a909e0b37214dd47db122bb153495ff0b02e9e54a50dbe16" },
        { 255, "436f756e742d323535", "18ab939d63ddec9f6ac2b60d61d36a7375d2070c9b683861110757062c52b8880a5f6b3936da9cd6c23ef2a95c" },
        { 256, "436f756e742d323536", "7a4a13e9ef23978e2c520fd4d2e757514ae160cd0cd05e556ef692370ca53076214c0c40d4c728d6ed9e727a5b" },
    };
    unsigned char pt[64], aad[16], ct[128], back[128];
    size_t pt_len = unhex("4265617574792069732074727574682c20747275746820626561757479", pt);
    for (size_t i = 0; i < sizeof enc_vectors / sizeof enc_vectors[0]; i++) {
        size_t aad_len = unhex(enc_vectors[i].aad, aad);
        s.seq = r.seq = enc_vectors[i].seq;
        CHECK(puddle_hpke_seal(&s, aad, aad_len, pt, pt_len, ct) == 0);
        CHECK(same_hex(ct, pt_len + PUDDLE_SEAL_TAG_LEN, enc_vectors[i].ct));
        CHECK(puddle_hpke_open(&r, aad, aad_len, ct, pt_len + PUDDLE_SEAL_TAG_LEN, back) == 0 && memcmp(back, pt, pt_len) == 0);
        CHECK(s.seq == enc_vectors[i].seq + 1 && r.seq == s.seq);
    }

    /* exports */
    unsigned char out[32], label[16];
    CHECK(puddle_hpke_export(&s, "", 0, out, 32) == 0);
    CHECK(same_hex(out, 32, "4bbd6243b8bb54cec311fac9df81841b6fd61f56538a775e7c80a9f40160606e"));
    label[0] = 0;
    CHECK(puddle_hpke_export(&s, label, 1, out, 32) == 0);
    CHECK(same_hex(out, 32, "8c1df14732580e5501b00f82b10a1647b40713191b7c1240ac80e2b68808ba69"));
    size_t ll = unhex("54657374436f6e74657874", label);
    CHECK(puddle_hpke_export(&r, label, ll, out, 32) == 0);
    CHECK(same_hex(out, 32, "5acb09211139c43b3090489a9da433e8a30ee7188ba8b0a9a1ccf0c229283e53"));
}

/* a fresh one-time key each time; what doesn't belong to the context doesn't open */
static void test_round_trip(void)
{
    unsigned char sk[32], pk[32], other_sk[32], other_pk[32], enc[32], enc2[32];
    CHECK(puddle_x25519_derive("recipient key for the round trip", 32, sk, pk) == 0);
    CHECK(puddle_x25519_derive("someone else entirely, not them.", 32, other_sk, other_pk) == 0);
    puddle_hpke s, s2, r, wrong;
    const char *info = "puddle-seal-v1\nalice\nbob\nnonce";
    CHECK(puddle_hpke_sender(pk, info, strlen(info), enc, &s) == 0);
    CHECK(puddle_hpke_sender(pk, info, strlen(info), enc2, &s2) == 0);
    CHECK(memcmp(enc, enc2, 32) != 0);

    const char *msg = "{\"events\":[\"hello\"]}";
    size_t n = strlen(msg);
    unsigned char ct[64], pt[64];
    CHECK(puddle_hpke_seal(&s, "POST\n/stream/v1/events", 22, msg, n, ct) == 0);

    CHECK(puddle_hpke_receiver(enc, sk, info, strlen(info), &r) == 0);
    puddle_hpke keep = r;
    CHECK(puddle_hpke_open(&r, "POST\n/stream/v1/events", 22, ct, n + 16, pt) == 0 && memcmp(pt, msg, n) == 0);
    r = keep;
    CHECK(puddle_hpke_open(&r, "POST\n/stream/v1/close", 21, ct, n + 16, pt) == -1);          /* another route */
    r = keep;
    ct[3] ^= 1;
    CHECK(puddle_hpke_open(&r, "POST\n/stream/v1/events", 22, ct, n + 16, pt) == -1);         /* changed on the way */
    ct[3] ^= 1;
    CHECK(puddle_hpke_receiver(enc, other_sk, info, strlen(info), &wrong) == 0);
    CHECK(puddle_hpke_open(&wrong, "POST\n/stream/v1/events", 22, ct, n + 16, pt) == -1);     /* not theirs to open */
    CHECK(puddle_hpke_receiver(enc, sk, "puddle-seal-v1\nalice\ncarol\nnonce", 32, &wrong) == 0);
    CHECK(puddle_hpke_open(&wrong, "POST\n/stream/v1/events", 22, ct, n + 16, pt) == -1);     /* meant for another request */
    CHECK(puddle_hpke_open(&keep, "", 0, ct, 15, pt) == -1);                                  /* shorter than a tag */

    unsigned char zero[32] = { 0 };
    CHECK(puddle_hpke_sender(zero, info, strlen(info), enc, &s) == -1); /* a key of low order */
}

static void answer_round_trip(const unsigned char key[32], size_t len)
{
    unsigned char *pt = malloc(len + 1), *ct = malloc(puddle_seal_answer_len(len)), *back = malloc(len + 1);
    for (size_t i = 0; i < len; i++)
        pt[i] = (unsigned char)(i * 7 + 3);
    size_t sealed = puddle_seal_answer_len(len), got = 999;
    CHECK(puddle_seal_plain_len(sealed) == len);
    CHECK(puddle_seal_answer(key, 200, pt, len, ct) == 0);
    CHECK(puddle_open_answer(key, 200, ct, sealed, back, &got) == 0 && got == len && (len == 0 || memcmp(back, pt, len) == 0));
    CHECK(puddle_open_answer(key, 404, ct, sealed, back, &got) == -1);                          /* another status */
    if (sealed > PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN) {                                      /* cut after a whole record */
        size_t cut = PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN;
        CHECK(puddle_open_answer(key, 200, ct, cut, back, &got) == -1);
    }
    if (sealed > 1)
        CHECK(puddle_open_answer(key, 200, ct, sealed - 1, back, &got) == -1);                 /* a byte short */
    free(pt);
    free(ct);
    free(back);
}

static void test_answers(void)
{
    unsigned char sk[32], pk[32], enc[32], ks[32], kr[32];
    CHECK(puddle_x25519_derive("the answer test's recipient key!", 32, sk, pk) == 0);
    puddle_hpke s, r;
    CHECK(puddle_hpke_sender(pk, "i", 1, enc, &s) == 0 && puddle_hpke_receiver(enc, sk, "i", 1, &r) == 0);
    CHECK(puddle_seal_answer_key(&s, ks) == 0 && puddle_seal_answer_key(&r, kr) == 0 && memcmp(ks, kr, 32) == 0);

    CHECK(puddle_seal_answer_len(0) == 16 && puddle_seal_answer_len(1) == 17);
    CHECK(puddle_seal_answer_len(PUDDLE_SEAL_RECORD) == PUDDLE_SEAL_RECORD + 16);
    CHECK(puddle_seal_answer_len(PUDDLE_SEAL_RECORD + 1) == PUDDLE_SEAL_RECORD + 1 + 32);
    CHECK(puddle_seal_plain_len(15) == 0 && puddle_seal_plain_len(16) == 0);
    size_t sizes[] = { 0, 1, 100, PUDDLE_SEAL_RECORD - 1, PUDDLE_SEAL_RECORD, PUDDLE_SEAL_RECORD + 1, 3 * PUDDLE_SEAL_RECORD + 5 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        answer_round_trip(ks, sizes[i]);

    /* records can't be reordered: each is bound to its place */
    unsigned char a[PUDDLE_SEAL_RECORD + 16], out[PUDDLE_SEAL_RECORD];
    memset(out, 1, sizeof out);
    CHECK(puddle_seal_record(ks, 0, 200, 0, out, PUDDLE_SEAL_RECORD, a) == 0);
    CHECK(puddle_open_record(ks, 1, 200, 0, a, sizeof a, out) == -1);
    CHECK(puddle_open_record(ks, 0, 200, 1, a, sizeof a, out) == -1);
    CHECK(puddle_open_record(ks, 0, 200, 0, a, sizeof a, out) == 0);
}

int main(void)
{
    test_rfc9180();
    test_round_trip();
    test_answers();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
