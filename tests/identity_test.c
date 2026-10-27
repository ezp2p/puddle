#include "core/identity.h"

#include "base/encoding.h"
#include "base/hash.h"

#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];

static char *dir_for(const char *name)
{
    static char p[600];
    snprintf(p, sizeof p, "%s/%s", root, name);
    return p;
}

static char *file_text(const char *dir, const char *name, size_t *len)
{
    char path[700];
    snprintf(path, sizeof path, "%s/identity/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(1 << 16);
    *len = fread(buf, 1, (1 << 16) - 1, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static void write_text(const char *dir, const char *name, const char *text)
{
    char path[700];
    snprintf(path, sizeof path, "%s/identity/%s", dir, name);
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static size_t lines(const char *t)
{
    size_t n = 0;
    for (; *t; t++) if (*t == '\n') n++;
    return n;
}

static int verify(const char *text, const char *expect_id, puddle_chain_head *h, char *err)
{
    return puddle_chain_verify(text, strlen(text), expect_id, h, err, 200);
}

/* an identity as the rest of the system sees it: made, and its phrase acknowledged */
static puddle_identity *create_ok(const char *dir, char phrase[PUDDLE_PHRASE_LEN + 1], char *err, size_t n)
{
    puddle_identity *id = puddle_identity_create(dir, phrase, err, n);
    if (id) puddle_identity_confirm(dir);
    return id;
}

static void unhex(const char *hex, unsigned char *out, size_t n)
{
    if (puddle_hex_decode(hex, out, n) != 0) { fprintf(stderr, "bad test vector\n"); exit(2); }
}

/* ---- is a secret anywhere under a directory? ---- */

static const char *needles[16];
static int nneedles, found_in_files;

static int scan_cb(const char *path, const struct stat *sb, int type, struct FTW *f)
{
    (void)sb; (void)f;
    if (type != FTW_F) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char *buf = malloc(1 << 16);
    size_t n = fread(buf, 1, (1 << 16) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    for (int i = 0; i < nneedles; i++)
        if (memmem(buf, n, needles[i], strlen(needles[i]))) {
            fprintf(stderr, "secret %d found in %s\n", i, path);
            found_in_files++;
        }
    free(buf);
    return 0;
}

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

/* The keys made from a fixed phrase, worked out independently (HKDF in Python, public keys by the
 * openssl command line). If these change, every existing identity stops matching its phrase. */
#define KAT_PHRASE "0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406"
#define KAT_SIGN "a608989739fe6a72e390aaff469b13784d623a835d55d7fd4fb0374e7187d328"
#define KAT_ENC  "2a2d8b337a9af9734a3469e985292e62103548c9d8409b79498cdcf47def3d08"
#define KAT_ROLL "1e9ce499e8ef59ae734002c67857c5a2f2cce1646cfc0d58ab885c7a682f6fd2"
#define KAT_ID   "yvq4bfjwaelmoies6wjziyscagw3cjjjygqvhrw237b2mkcqsjya"
#define KAT_NEXT "6379abf92797ab8fd67ccf291c35c51c20fd6172b218e2f6620491c9ecaef50a"

static void test_known_answer(void)
{
    puddle_phrase_keys k;
    unsigned char want[32];
    CHECK(puddle_identity_keys_from_phrase(KAT_PHRASE, &k) == 0);
    unhex(KAT_SIGN, want, 32); CHECK(memcmp(k.sign_pub, want, 32) == 0);
    unhex(KAT_ENC, want, 32);  CHECK(memcmp(k.enc_pub, want, 32) == 0);
    unhex(KAT_ROLL, want, 32); CHECK(memcmp(k.roll_pub, want, 32) == 0);

    char hash[65];
    unsigned char both[64], md[32];
    memcpy(both, k.sign_pub, 32);
    puddle_sha256_hex(k.roll_pub, 32, hash);                 /* the commitment to the roll key */
    CHECK(strcmp(hash, KAT_NEXT) == 0);
    unhex(hash, both + 32, 32);
    char hash2[65];
    puddle_sha256_hex(both, 64, hash2);                      /* the id: the signing key AND that commitment */
    unhex(hash2, md, 32);
    char id[53];
    puddle_base32_encode(md, 32, id, sizeof id);
    CHECK(strcmp(id, KAT_ID) == 0);

    /* any spelling of the phrase gives the same keys; a bad phrase gives none */
    puddle_phrase_keys k2;
    CHECK(puddle_identity_keys_from_phrase("0123 4567 89ab cdef ghjk mnpq rstv g406", &k2) == 0 && memcmp(&k, &k2, sizeof k) == 0);
    CHECK(puddle_identity_keys_from_phrase("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G407", &k2) != 0);
    CHECK(puddle_identity_keys_from_phrase("", &k2) != 0);
}

static void test_create_and_reopen(void)
{
    char err[600], phrase[PUDDLE_PHRASE_LEN + 1], canon[PUDDLE_PHRASE_LEN + 1];
    char *d = dir_for("a");

    CHECK(!puddle_identity_exists(d));
    puddle_identity *none = puddle_identity_open(d, err, sizeof err);
    CHECK(none == NULL && strstr(err, "--init") != NULL);

    puddle_identity *a = puddle_identity_create(d, phrase, err, sizeof err);
    CHECK(a != NULL);
    CHECK(puddle_identity_exists(d));
    CHECK(puddle_phrase_parse(phrase, canon) == PUDDLE_PHRASE_OK && strcmp(phrase, canon) == 0);

    char id[53];
    unsigned char sp[32], ep[32];
    uint64_t seq;
    puddle_identity_id(a, id);
    puddle_identity_public(a, sp, ep, &seq);
    CHECK(strlen(id) == 52 && seq == 0);

    /* everything comes from the phrase: the keys and the id are exactly what the phrase gives */
    puddle_phrase_keys k;
    CHECK(puddle_identity_keys_from_phrase(phrase, &k) == 0);
    CHECK(memcmp(sp, k.sign_pub, 32) == 0 && memcmp(ep, k.enc_pub, 32) == 0);
    char hash[65], hash2[65];
    unsigned char md[32], both[64];
    char want_id[53];
    memcpy(both, k.sign_pub, 32);
    puddle_sha256_hex(k.roll_pub, 32, hash);
    unhex(hash, both + 32, 32);
    puddle_sha256_hex(both, 64, hash2);
    unhex(hash2, md, 32);
    puddle_base32_encode(md, 32, want_id, sizeof want_id);
    CHECK(strcmp(id, want_id) == 0);

    /* the chain vouches for them, and commits to the roll key */
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_chain_head h;
    char verr[300];
    CHECK(chain && puddle_chain_verify(chain, cl, id, &h, verr, sizeof verr) == 0);
    CHECK(h.seq == 0 && memcmp(h.sign_pub, sp, 32) == 0 && memcmp(h.enc_pub, ep, 32) == 0);
    puddle_sha256_hex(k.roll_pub, 32, hash);
    CHECK(strcmp(h.next, hash) == 0);
    CHECK(lines(chain) == 1);
    free(chain);

    /* private files */
    struct stat st;
    char path[700];
    snprintf(path, sizeof path, "%s/identity/keys", d);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* neither the phrase nor the roll key, nor the seed they come from, is anywhere on disk */
    unsigned char seed[32], roll_priv[32], sign_priv[32];
    CHECK(puddle_phrase_seed(phrase, seed) == 0);
    puddle_hkdf_sha256(seed, 32, "puddle-keys-v1", "roll", roll_priv, 32);
    puddle_hkdf_sha256(seed, 32, "puddle-keys-v1", "signing", sign_priv, 32);
    char seed_hex[65], roll_hex[65], sign_hex[65], bare[40], lower[40];
    puddle_hex_encode(seed, 32, seed_hex);
    puddle_hex_encode(roll_priv, 32, roll_hex);
    puddle_hex_encode(sign_priv, 32, sign_hex);
    size_t o = 0;
    for (const char *c = phrase; *c; c++) if (*c != '-') bare[o++] = *c;
    bare[o] = '\0';
    for (size_t i = 0; i <= o; i++) lower[i] = (bare[i] >= 'A' && bare[i] <= 'Z') ? (char)(bare[i] + 32) : bare[i];
    nneedles = 0;
    needles[nneedles++] = phrase; needles[nneedles++] = bare; needles[nneedles++] = lower;
    needles[nneedles++] = seed_hex; needles[nneedles++] = roll_hex;
    found_in_files = 0;
    nftw(root, scan_cb, 16, FTW_PHYS);
    CHECK(found_in_files == 0);
    /* ...while the daily signing key, which does live on this machine, is there (so the scan works) */
    nneedles = 0; needles[nneedles++] = sign_hex;
    found_in_files = 0;
    nftw(d, scan_cb, 16, FTW_PHYS);
    CHECK(found_in_files == 1);
    size_t kl;
    char *keys = file_text(d, "keys", &kl);
    CHECK(lines(keys) == 4); /* header, id, signing, encryption: no roll key, no next keys */
    free(keys);
    memset(seed, 0, 32); memset(roll_priv, 0, 32); memset(sign_priv, 0, 32);

    /* signing works, and verifies against the public key and nothing else */
    unsigned char sig[64];
    CHECK(puddle_identity_sign(a, "hello", 5, sig) == 0);
    CHECK(puddle_verify_signature(sp, "hello", 5, sig));
    CHECK(!puddle_verify_signature(sp, "hellp", 5, sig));
    CHECK(!puddle_verify_signature(k.enc_pub, "hello", 5, sig));
    puddle_identity_close(a);

    /* never overwritten */
    char again[PUDDLE_PHRASE_LEN + 1];
    CHECK(puddle_identity_create(d, again, err, sizeof err) == NULL && strstr(err, "already exists"));

    /* a new identity can't be used until its phrase has been confirmed */
    CHECK(puddle_identity_unconfirmed(d));
    CHECK(puddle_identity_open(d, err, sizeof err) == NULL && strstr(err, "never confirmed"));
    CHECK(puddle_identity_confirm(d) == 0 && !puddle_identity_unconfirmed(d));
    CHECK(puddle_identity_confirm(d) == 0); /* saying so twice is fine */

    /* same identity next time */
    a = puddle_identity_open(d, err, sizeof err);
    CHECK(a != NULL);
    char id2[53];
    unsigned char sp2[32];
    puddle_identity_id(a, id2);
    puddle_identity_public(a, sp2, NULL, &seq);
    CHECK(strcmp(id, id2) == 0 && memcmp(sp, sp2, 32) == 0 && seq == 0);
    CHECK(puddle_identity_sign(a, "hello", 5, sig) == 0 && puddle_verify_signature(sp, "hello", 5, sig));
    puddle_identity_close(a);

    /* another Puddle: another phrase, another id */
    char phrase_b[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *b = puddle_identity_create(dir_for("b"), phrase_b, err, sizeof err);
    puddle_identity_id(b, id2);
    CHECK(strcmp(id, id2) != 0 && strcmp(phrase, phrase_b) != 0);
    puddle_identity_close(b);
}


static void test_chain_verify(void)
{
    char err[300], id[53], other[53], verr[300], phrase[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *a = puddle_identity_create(dir_for("chain-a"), phrase, err, sizeof err);
    puddle_identity_id(a, id);
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);
    puddle_identity *b = puddle_identity_create(dir_for("chain-b"), phrase, err, sizeof err);
    puddle_identity_id(b, other);
    size_t bl;
    char *bchain = puddle_identity_chain(b, &bl);
    puddle_identity_close(b);
    puddle_chain_head h;
    CHECK(verify(chain, id, &h, verr) == 0 && verify(chain, NULL, &h, verr) == 0);
    CHECK(verify(chain, other, &h, verr) == -1 && strstr(verr, "different id")); /* a link's id pins the key */

    char t[2000];
    snprintf(t, sizeof t, "%s", chain);                      /* a changed byte inside the record */
    char *p = strstr(t, "\"ts\":");
    p[5] = (p[5] == '1') ? '2' : '1';
    CHECK(verify(t, id, &h, verr) == -1 && strstr(verr, "bad signature"));
    snprintf(t, sizeof t, "%s", chain);                      /* a damaged signature */
    p = strstr(t, "\"sig\":\"") + 7;
    p[3] = (p[3] == 'A') ? 'B' : 'A';
    CHECK(verify(t, id, &h, verr) == -1);
    snprintf(t, sizeof t, "%s", chain);                      /* swap in someone else's key, keeping the signature */
    char *a_sign = strstr(t, "\"signing\":\"") + 11;
    char *b_sign = strstr(bchain, "\"signing\":\"") + 11;
    memcpy(a_sign, b_sign, 44);
    CHECK(verify(t, NULL, &h, verr) == -1 && strstr(verr, "bad signature"));
    snprintf(t, sizeof t, "%s", chain);                      /* change the roll commitment */
    p = strstr(t, "\"next\":\"") + 8;
    p[0] = (p[0] == '0') ? '1' : '0';
    CHECK(verify(t, id, &h, verr) == -1 && strstr(verr, "different id"));  /* the id commits to it, so it is a different id */
    CHECK(verify(t, NULL, &h, verr) == -1 && strstr(verr, "bad signature")); /* and with no id to check, the signature fails */
    snprintf(t, sizeof t, "%.*s", (int)(strlen(chain) - 40), chain);
    CHECK(verify(t, id, &h, verr) == -1);                    /* cut off mid-line */
    CHECK(puddle_chain_verify("", 0, NULL, &h, verr, sizeof verr) == -1);
    CHECK(verify("not json\n", NULL, &h, verr) == -1);
    CHECK(verify("{\"record\":{\"type\":\"identity.inception\"},\"sig\":\"AAAA\"}\n", NULL, &h, verr) == -1);

    snprintf(t, sizeof t, "%s", chain);                      /* wrong kind or number of record */
    p = strstr(t, "identity.inception");
    memcpy(p, "identity.inceptiox", 18);
    CHECK(verify(t, id, &h, verr) == -1);
    snprintf(t, sizeof t, "%s%s", chain, chain);             /* a second record that is not a rotation */
    CHECK(verify(t, id, &h, verr) == -1 && strstr(verr, "out of order"));

    free(chain);
    free(bchain);
}

static void test_damaged_and_old(void)
{
    char err[700], phrase[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *a;
    char path[800];
    size_t kl;

    /* keys file gone */
    a = create_ok(dir_for("nokeys"), phrase, err, sizeof err);
    puddle_identity_close(a);
    snprintf(path, sizeof path, "%s/identity/keys", dir_for("nokeys"));
    unlink(path);
    CHECK(puddle_identity_open(dir_for("nokeys"), err, sizeof err) == NULL && strstr(err, "missing"));

    /* chain gone, or empty */
    a = create_ok(dir_for("nochain"), phrase, err, sizeof err);
    puddle_identity_close(a);
    write_text(dir_for("nochain"), "chain.jsonl", "");
    CHECK(puddle_identity_open(dir_for("nochain"), err, sizeof err) == NULL && strstr(err, "missing or empty"));
    snprintf(path, sizeof path, "%s/identity/chain.jsonl", dir_for("nochain"));
    unlink(path);
    CHECK(puddle_identity_open(dir_for("nochain"), err, sizeof err) == NULL && strstr(err, "missing or empty"));

    /* damaged keys */
    a = create_ok(dir_for("damaged"), phrase, err, sizeof err);
    puddle_identity_close(a);
    write_text(dir_for("damaged"), "keys", "puddle-keys 2\nsigning 00\n");
    CHECK(puddle_identity_open(dir_for("damaged"), err, sizeof err) == NULL && strstr(err, "damaged"));

    /* keys that belong to a different identity */
    a = create_ok(dir_for("m1"), phrase, err, sizeof err); puddle_identity_close(a);
    a = create_ok(dir_for("m2"), phrase, err, sizeof err); puddle_identity_close(a);
    char *k2 = file_text(dir_for("m2"), "keys", &kl);
    write_text(dir_for("m1"), "keys", k2);
    CHECK(puddle_identity_open(dir_for("m1"), err, sizeof err) == NULL && strstr(err, "different identities"));
    free(k2);

    /* the id line says one thing and the keys another */
    a = create_ok(dir_for("m3"), phrase, err, sizeof err); puddle_identity_close(a);
    char *k1 = file_text(dir_for("m1"), "keys", &kl); /* m1's keys are now m2's; take m3's own, with m2's id */
    free(k1);
    char *k3 = file_text(dir_for("m3"), "keys", &kl);
    char *idline = strstr(k3, "id ") + 3;
    idline[0] = (idline[0] == 'a') ? 'b' : 'a';
    write_text(dir_for("m3"), "keys", k3);
    CHECK(puddle_identity_open(dir_for("m3"), err, sizeof err) == NULL);
    free(k3);

    /* the id line agrees with the chain, but the keys are someone else's */
    a = create_ok(dir_for("m4"), phrase, err, sizeof err); puddle_identity_close(a);
    a = create_ok(dir_for("m5"), phrase, err, sizeof err); puddle_identity_close(a);
    char *k4 = file_text(dir_for("m4"), "keys", &kl);
    char *k5 = file_text(dir_for("m5"), "keys", &kl);
    memcpy(strstr(k5, "id ") + 3, strstr(k4, "id ") + 3, PUDDLE_USER_ID_LEN); /* m4's id, m5's keys */
    write_text(dir_for("m4"), "keys", k5);
    CHECK(puddle_identity_open(dir_for("m4"), err, sizeof err) == NULL && strstr(err, "not the ones"));
    free(k4);
    free(k5);

    /* an identity from before there were phrases is refused, not silently replaced */
    mkdir(dir_for("old"), 0700);
    char oldid[700];
    snprintf(oldid, sizeof oldid, "%s/identity", dir_for("old"));
    mkdir(oldid, 0700);
    write_text(dir_for("old"), "keys", "puddle-keys 1\nsigning 00\nencryption 00\nnext_signing 00\nnext_encryption 00\n");
    write_text(dir_for("old"), "chain.jsonl", "{}\n");
    CHECK(puddle_identity_open(dir_for("old"), err, sizeof err) == NULL);
    CHECK(strstr(err, "older development version") && strstr(err, "--init") && strstr(err, "aside"));
    CHECK(puddle_identity_exists(dir_for("old")));
    CHECK(puddle_identity_create(dir_for("old"), phrase, err, sizeof err) == NULL); /* and nothing is overwritten */
    char *still = file_text(dir_for("old"), "keys", &kl);
    CHECK(still && strncmp(still, "puddle-keys 1", 13) == 0);
    free(still);

    /* a half-made identity from a crashed run is not an identity, and doesn't get in the way */
    char leftover[700];
    snprintf(leftover, sizeof leftover, "%s/identity.new-99999", dir_for("crashed"));
    mkdir(dir_for("crashed"), 0700);
    mkdir(leftover, 0700);
    CHECK(!puddle_identity_exists(dir_for("crashed")));
    a = puddle_identity_create(dir_for("crashed"), phrase, err, sizeof err);
    CHECK(a != NULL && puddle_identity_exists(dir_for("crashed")));
    puddle_identity_close(a);
}


/* The owner quit while the phrase was on screen: the identity exists but nobody can have written the
 * phrase down. It must not be used, and it must be possible to start again without harming a real one. */
static void test_unconfirmed(void)
{
    char err[700], p1[PUDDLE_PHRASE_LEN + 1], p2[PUDDLE_PHRASE_LEN + 1];
    char *d = dir_for("unc");
    char path[800];

    CHECK(puddle_identity_confirm(d) == -1);                         /* nothing to confirm */
    CHECK(puddle_identity_discard_unconfirmed(d, err, sizeof err) == 1); /* nothing to discard */

    puddle_identity *a = puddle_identity_create(d, p1, err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_close(a);                                        /* "quit" before confirming */
    snprintf(path, sizeof path, "%s/identity/unconfirmed", d);
    CHECK(access(path, F_OK) == 0 && puddle_identity_unconfirmed(d));
    CHECK(puddle_identity_open(d, err, sizeof err) == NULL && strstr(err, "never confirmed") && strstr(err, "puddled --init"));
    CHECK(puddle_identity_create(d, p2, err, sizeof err) == NULL && strstr(err, "already exists")); /* not overwritten, even so */

    /* start again: the unfinished one goes, a new one (new phrase, new id) takes its place */
    puddle_phrase_keys first;
    CHECK(puddle_identity_keys_from_phrase(p1, &first) == 0);
    CHECK(puddle_identity_discard_unconfirmed(d, err, sizeof err) == 0);
    CHECK(!puddle_identity_exists(d) && !puddle_identity_unconfirmed(d));
    a = puddle_identity_create(d, p2, err, sizeof err);
    CHECK(a != NULL && strcmp(p1, p2) != 0);
    unsigned char sp[32];
    puddle_identity_public(a, sp, NULL, NULL);
    CHECK(memcmp(sp, first.sign_pub, 32) != 0);
    puddle_identity_close(a);

    /* once confirmed it is a real identity, and discarding it is refused and changes nothing */
    CHECK(puddle_identity_confirm(d) == 0 && access(path, F_OK) != 0);
    size_t before_len, after_len;
    char *before = file_text(d, "chain.jsonl", &before_len);
    char *keys_before = file_text(d, "keys", &after_len);
    CHECK(puddle_identity_discard_unconfirmed(d, err, sizeof err) == 1);
    CHECK(puddle_identity_exists(d));
    char *after = file_text(d, "chain.jsonl", &after_len);
    CHECK(after && before && after_len == before_len && memcmp(before, after, before_len) == 0);
    a = puddle_identity_open(d, err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_close(a);
    free(before); free(after); free(keys_before);
}


/* ============================ rolling ============================ */

/* A test-side copy of everything one phrase gives, with the private halves, so records can be built by hand
 * (including forged ones). It repeats the derivation labels on purpose: a change to them must break this. */
typedef struct {
    unsigned char sign_priv[32], sign_pub[32], enc_pub[32], roll_priv[32], roll_pub[32];
} tgen;

static void pub_of(int type, const unsigned char priv[32], unsigned char pub[32])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(type, NULL, priv, 32);
    size_t n = 32;
    EVP_PKEY_get_raw_public_key(k, pub, &n);
    EVP_PKEY_free(k);
}

static void gen_of(const char *phrase, tgen *g)
{
    unsigned char seed[32], enc_priv[32];
    puddle_phrase_seed(phrase, seed);
    puddle_hkdf_sha256(seed, 32, "puddle-keys-v1", "signing", g->sign_priv, 32);
    puddle_hkdf_sha256(seed, 32, "puddle-keys-v1", "encryption", enc_priv, 32);
    puddle_hkdf_sha256(seed, 32, "puddle-keys-v1", "roll", g->roll_priv, 32);
    pub_of(EVP_PKEY_ED25519, g->sign_priv, g->sign_pub);
    pub_of(EVP_PKEY_X25519, enc_priv, g->enc_pub);
    pub_of(EVP_PKEY_ED25519, g->roll_priv, g->roll_pub);
    memset(seed, 0, 32);
}

static void ed_sign(const unsigned char priv[32], const char *data, size_t len, char out64[100])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32);
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned char sig[64];
    size_t sl = 64;
    EVP_DigestSignInit(c, NULL, NULL, NULL, k);
    EVP_DigestSign(c, sig, &sl, (const unsigned char *)data, len);
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    puddle_base64_encode(sig, 64, out64, 100);
}

static void b64(const unsigned char *in, char *out) { puddle_base64_encode(in, 32, out, 64); }

/* A rotation line built by hand. `new_keys` are announced; `revealed` is the roll key shown (and whose public
 * half is in the record); `commit_to` is the generation whose roll key the record commits to next. The
 * three signers are whoever the caller says, which is how forgeries are made. */
static void rot_line(char *out, size_t cap, uint64_t seq, const char *prev_hash, const tgen *new_keys,
                     const tgen *revealed, const tgen *commit_to, const unsigned char *new_signer,
                     const unsigned char *roll_signer, const unsigned char *prev_signer)
{
    char sp[64], ep[64], rp[64], next[65], rec[700], s1[100], s2[100], s3[100];
    b64(new_keys->sign_pub, sp); b64(new_keys->enc_pub, ep); b64(revealed->roll_pub, rp);
    puddle_sha256_hex(commit_to->roll_pub, 32, next);
    int n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.rotate\",\"seq\":%llu,\"prev\":\"%s\",\"signing\":\"%s\",\"encryption\":\"%s\",\"roll\":\"%s\",\"next\":\"%s\",\"ts\":1}",
                     (unsigned long long)seq, prev_hash, sp, ep, rp, next);
    ed_sign(new_signer, rec, (size_t)n, s1);
    ed_sign(roll_signer, rec, (size_t)n, s2);
    ed_sign(prev_signer, rec, (size_t)n, s3);
    snprintf(out, cap, "{\"record\":%s,\"sig\":\"%s\",\"sig_roll\":\"%s\",\"sig_prev\":\"%s\"}\n", rec, s1, s2, s3);
}

/* hash of the last record of a chain, the way the next record must name it */
static void head_hash_of(const char *chain, char out[65])
{
    puddle_chain_head h;
    char e[200];
    if (puddle_chain_verify(chain, strlen(chain), NULL, &h, e, sizeof e) != 0) { fprintf(stderr, "chain: %s\n", e); exit(2); }
    memcpy(out, h.head_hash, 65);
}

/* A thief with the daily keys of a never-changed identity signs a DIFFERENT first record: same signing key, but
 * committing to a roll key of his own. It must not be a valid chain for the victim's id. */
static void test_forged_inception(void)
{
    char err[600], p0[PUDDLE_PHRASE_LEN + 1], px[PUDDLE_PHRASE_LEN + 1], verr[300];
    puddle_identity *a = create_ok(dir_for("forge-first"), p0, err, sizeof err);
    char id[53];
    puddle_identity_id(a, id);
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);
    tgen g0, gx;
    puddle_phrase_generate(px);
    gen_of(p0, &g0);
    gen_of(px, &gx);

    char sp[64], ep[64], next[65], rec[500], sig[100], line[800];
    b64(g0.sign_pub, sp); b64(g0.enc_pub, ep);                    /* the victim's real daily keys... */
    puddle_sha256_hex(gx.roll_pub, 32, next);                      /* ...but the thief's own roll key */
    int n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.inception\",\"seq\":0,\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":1}",
                     sp, ep, next);
    ed_sign(g0.sign_priv, rec, (size_t)n, sig);                    /* validly signed by the stolen key */
    snprintf(line, sizeof line, "{\"record\":%s,\"sig\":\"%s\"}\n", rec, sig);

    puddle_chain_head h;
    CHECK(verify(line, NULL, &h, verr) == 0);                      /* it is a well-formed chain, for SOME id... */
    CHECK(strcmp(h.id, id) != 0);                                  /* ...but not the victim's */
    CHECK(verify(line, id, &h, verr) == -1 && strstr(verr, "different id")); /* so a link's id rejects it */
    CHECK(verify(chain, id, &h, verr) == 0);                       /* while the real one still verifies */

    /* and it can't be fed to the owner either: their own phrase matches its keys, but not what it commits to */
    char rdir[600];
    snprintf(rdir, sizeof rdir, "%s", dir_for("forge-restore"));
    CHECK(puddle_identity_restore(rdir, p0, line, strlen(line), err, sizeof err) == NULL && strstr(err, "doesn't belong"));
    CHECK(!puddle_identity_exists(rdir));
    puddle_identity *back = puddle_identity_restore(rdir, p0, chain, cl, err, sizeof err); /* the real one works */
    CHECK(back != NULL);
    puddle_identity_close(back);
    free(chain);
}

static void test_rotation_rules(void)
{
    char err[600], p0[PUDDLE_PHRASE_LEN + 1], p1[PUDDLE_PHRASE_LEN + 1], px[PUDDLE_PHRASE_LEN + 1], verr[300];
    puddle_identity *a = create_ok(dir_for("rules"), p0, err, sizeof err);
    char id[53];
    puddle_identity_id(a, id);
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);
    puddle_phrase_generate(p1);
    puddle_phrase_generate(px);
    tgen g0, g1, gx;
    gen_of(p0, &g0); gen_of(p1, &g1); gen_of(px, &gx);
    char head[65], line[1400], text[6000];
    head_hash_of(chain, head);
    puddle_chain_head h;

    /* control: a rotation by the book is accepted and the head moves to the new keys */
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == 0);
    CHECK(h.seq == 1 && memcmp(h.sign_pub, g1.sign_pub, 32) == 0 && memcmp(h.enc_pub, g1.enc_pub, 32) == 0);
    char want[65];
    puddle_sha256_hex(g1.roll_pub, 32, want);
    CHECK(strcmp(h.next, want) == 0);
    CHECK(strcmp(h.id, id) == 0);                            /* the id is still the first key's */

    /* THE THIEF: holds the daily keys (so can sign as the current key) but not the roll key. He announces keys
     * of his own, signs as the new key and as the stolen old key, and offers his own roll key. */
    rot_line(line, sizeof line, 1, head, &gx, &gx, &gx, gx.sign_priv, gx.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "not the one the previous record committed to"));
    /* ...or passes the stolen signing key off as the roll key, which does sign correctly */
    tgen fake = gx;
    memcpy(fake.roll_pub, g0.sign_pub, 32);
    memcpy(fake.roll_priv, g0.sign_priv, 32);
    rot_line(line, sizeof line, 1, head, &gx, &fake, &gx, gx.sign_priv, fake.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "not the one the previous record committed to"));

    /* A LEAKED OLD PHRASE after a roll: the roll key was spent, so it can't authorize another */
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    char first_roll[1400];
    snprintf(first_roll, sizeof first_roll, "%s", line);
    char after_roll[3000];
    snprintf(after_roll, sizeof after_roll, "%s%s", chain, first_roll);
    char head1[65];
    head_hash_of(after_roll, head1);
    rot_line(line, sizeof line, 2, head1, &gx, &g0, &gx, gx.sign_priv, g0.roll_priv, g1.sign_priv); /* g0's roll key again */
    snprintf(text, sizeof text, "%s%s", after_roll, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "not the one the previous record committed to"));

    /* each of the three signatures is required, and each must be from the right key */
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, gx.sign_priv, g0.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "new key"));
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, gx.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "roll key"));
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, gx.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "previous key"));
    /* a record missing one of them */
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    char *cut = strstr(line, ",\"sig_prev\"");
    *cut = '}'; cut[1] = '\n'; cut[2] = '\0';
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "malformed"));

    /* order and linkage */
    rot_line(line, sizeof line, 1, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    char t2[6000];
    snprintf(t2, sizeof t2, "%s", text);
    char *ts = strstr(t2 + strlen(chain), "\"ts\":1");          /* a record changed after it was signed */
    ts[5] = '2';
    CHECK(verify(t2, id, &h, verr) == -1);
    char zeros[65];
    memset(zeros, '0', 64); zeros[64] = '\0';
    rot_line(line, sizeof line, 1, zeros, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "does not follow"));
    rot_line(line, sizeof line, 2, head, &g1, &g0, &g1, g1.sign_priv, g0.roll_priv, g0.sign_priv);
    snprintf(text, sizeof text, "%s%s", chain, line);
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "out of order"));
    snprintf(text, sizeof text, "%s%s%s", chain, first_roll, first_roll); /* the same rotation twice */
    CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "out of order"));

    /* the rotation can't be moved onto someone else's chain, nor their id claimed */
    char ph[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *b = create_ok(dir_for("rules-b"), ph, err, sizeof err);
    char bid[53];
    puddle_identity_id(b, bid);
    size_t bl;
    char *bchain = puddle_identity_chain(b, &bl);
    puddle_identity_close(b);
    snprintf(text, sizeof text, "%s%s", bchain, first_roll);
    CHECK(verify(text, NULL, &h, verr) == -1);
    snprintf(text, sizeof text, "%s%s", chain, first_roll);
    CHECK(verify(text, bid, &h, verr) == -1 && strstr(verr, "different id"));
    free(bchain);
    free(chain);
}

static size_t file_len(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : (size_t)-1;
}

static void test_roll_flow(void)
{
    char err[700], p0[PUDDLE_PHRASE_LEN + 1], p1[PUDDLE_PHRASE_LEN + 1], p2[PUDDLE_PHRASE_LEN + 1];
    char *d = dir_for("roll");
    puddle_identity *a = create_ok(d, p0, err, sizeof err);
    char id0[53], id1[53];
    puddle_identity_id(a, id0);
    unsigned char sp0[32], ep0[32], sp1[32], ep1[32];
    uint64_t seq;
    puddle_identity_public(a, sp0, ep0, &seq);
    unsigned char sig_old[64];
    puddle_identity_sign(a, "before", 6, sig_old);
    size_t c0len, k0len;
    char *chain0 = puddle_identity_chain(a, &c0len);
    char *keys0 = file_text(d, "keys", &k0len);

    /* things that are not the current phrase are refused, with a reason */
    char junk[PUDDLE_PHRASE_LEN + 1], typo[PUDDLE_PHRASE_LEN + 1], stranger[PUDDLE_PHRASE_LEN + 1];
    snprintf(typo, sizeof typo, "%s", p0);
    typo[0] = (typo[0] == '0') ? '1' : '0';
    puddle_phrase_generate(stranger);
    snprintf(junk, sizeof junk, "hello");
    CHECK(puddle_identity_roll_prepare(a, junk, p1, err, sizeof err) == NULL && strstr(err, "doesn't look like"));
    CHECK(puddle_identity_roll_prepare(a, typo, p1, err, sizeof err) == NULL && strstr(err, "typo"));
    CHECK(puddle_identity_roll_prepare(a, stranger, p1, err, sizeof err) == NULL && strstr(err, "not this identity's current"));
    CHECK(puddle_identity_roll_prepare(a, "", p1, err, sizeof err) == NULL);

    /* phase 1 changes nothing on disk or in memory, and abandoning costs nothing */
    puddle_roll *r = puddle_identity_roll_prepare(a, p0, p1, err, sizeof err);
    CHECK(r != NULL && puddle_phrase_parse(p1, NULL) == PUDDLE_PHRASE_OK && strcmp(p0, p1) != 0);
    unsigned char spn[32];
    puddle_identity_public(a, spn, NULL, &seq);
    CHECK(seq == 0 && memcmp(spn, sp0, 32) == 0);
    size_t c, k;
    char *same_c = puddle_identity_chain(a, &c);
    char *same_k = file_text(d, "keys", &k);
    CHECK(c == c0len && memcmp(same_c, chain0, c) == 0 && k == k0len && memcmp(same_k, keys0, k) == 0);
    free(same_c); free(same_k);
    CHECK(file_len(dir_for("roll")) != (size_t)-1);
    puddle_roll_abandon(r);
    r = puddle_identity_roll_prepare(a, p0, p1, err, sizeof err); /* the old phrase still works */
    CHECK(r != NULL);

    /* phrase typed in any spelling */
    char lazy[PUDDLE_PHRASE_LEN + 1];
    for (size_t i = 0; i <= strlen(p0); i++) lazy[i] = (p0[i] >= 'A' && p0[i] <= 'Z') ? (char)(p0[i] + 32) : p0[i];
    puddle_roll *r2 = puddle_identity_roll_prepare(a, lazy, p2, err, sizeof err);
    CHECK(r2 != NULL);

    /* phase 2 */
    CHECK(puddle_identity_roll_commit(a, r, err, sizeof err) == 0);
    puddle_identity_id(a, id1);
    puddle_identity_public(a, sp1, ep1, &seq);
    CHECK(strcmp(id0, id1) == 0 && seq == 1);                    /* same person */
    CHECK(memcmp(sp0, sp1, 32) != 0 && memcmp(ep0, ep1, 32) != 0);
    puddle_phrase_keys k1;
    CHECK(puddle_identity_keys_from_phrase(p1, &k1) == 0);
    CHECK(memcmp(sp1, k1.sign_pub, 32) == 0 && memcmp(ep1, k1.enc_pub, 32) == 0); /* the new keys come from the NEW phrase */

    /* the other prepared roll was against the old head: it can't be committed now */
    CHECK(puddle_identity_roll_commit(a, r2, err, sizeof err) == -1 && strstr(err, "changed since"));
    puddle_roll_abandon(r2);

    unsigned char sig_new[64];
    CHECK(puddle_identity_sign(a, "after", 5, sig_new) == 0);
    CHECK(puddle_verify_signature(sp1, "after", 5, sig_new) && !puddle_verify_signature(sp0, "after", 5, sig_new));
    CHECK(puddle_verify_signature(sp0, "before", 6, sig_old));    /* what the old key signed still verifies */

    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_chain_head h;
    char verr[300];
    CHECK(puddle_chain_verify(chain, cl, id0, &h, verr, sizeof verr) == 0 && h.seq == 1 && lines(chain) == 2);
    CHECK(memcmp(h.sign_pub, sp1, 32) == 0);
    CHECK(strncmp(chain, chain0, c0len) == 0);                    /* the old record is untouched; only appended to */
    free(chain);
    CHECK(access(dir_for("roll"), F_OK) == 0);
    char path[800];
    snprintf(path, sizeof path, "%s/identity/roll.pending", d);
    CHECK(access(path, F_OK) != 0);                               /* the journal is gone */
    puddle_identity_close(a);

    /* the old private key is gone from this machine, along with everything that could remake it */
    unsigned char seed0[32], seed1[32], old_sign[32], old_roll[32], new_sign[32];
    puddle_phrase_seed(p0, seed0); puddle_phrase_seed(p1, seed1);
    puddle_hkdf_sha256(seed0, 32, "puddle-keys-v1", "signing", old_sign, 32);
    puddle_hkdf_sha256(seed0, 32, "puddle-keys-v1", "roll", old_roll, 32);
    puddle_hkdf_sha256(seed1, 32, "puddle-keys-v1", "signing", new_sign, 32);
    char o_sign[65], o_roll[65], n_sign[65], s0[65], s1[65], bare0[40], bare1[40];
    puddle_hex_encode(old_sign, 32, o_sign); puddle_hex_encode(old_roll, 32, o_roll); puddle_hex_encode(new_sign, 32, n_sign);
    puddle_hex_encode(seed0, 32, s0); puddle_hex_encode(seed1, 32, s1);
    size_t o = 0;
    for (const char *q = p0; *q; q++) if (*q != '-') bare0[o++] = *q;
    bare0[o] = '\0'; o = 0;
    for (const char *q = p1; *q; q++) if (*q != '-') bare1[o++] = *q;
    bare1[o] = '\0';
    nneedles = 0;
    needles[nneedles++] = o_sign; needles[nneedles++] = o_roll; needles[nneedles++] = s0; needles[nneedles++] = s1;
    needles[nneedles++] = p0; needles[nneedles++] = p1; needles[nneedles++] = bare0; needles[nneedles++] = bare1;
    found_in_files = 0;
    nftw(root, scan_cb, 16, FTW_PHYS);
    CHECK(found_in_files == 0);
    nneedles = 0; needles[nneedles++] = n_sign;                    /* the new daily key is on disk, so the scan is sound */
    found_in_files = 0;
    nftw(d, scan_cb, 16, FTW_PHYS);
    CHECK(found_in_files == 1);
    memset(old_sign, 0, 32); memset(old_roll, 0, 32); memset(new_sign, 0, 32); memset(seed0, 0, 32); memset(seed1, 0, 32);

    /* it survives a restart, and now only the NEW phrase can roll */
    a = puddle_identity_open(d, err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_public(a, NULL, NULL, &seq);
    CHECK(seq == 1);
    CHECK(puddle_identity_roll_prepare(a, p0, p2, err, sizeof err) == NULL && strstr(err, "not this identity's current"));
    r = puddle_identity_roll_prepare(a, p1, p2, err, sizeof err);
    CHECK(r != NULL && strcmp(p2, p1) != 0 && strcmp(p2, p0) != 0);
    CHECK(puddle_identity_roll_commit(a, r, err, sizeof err) == 0);
    puddle_identity_public(a, NULL, NULL, &seq);
    CHECK(seq == 2);
    CHECK(puddle_identity_roll_prepare(a, p1, p0, err, sizeof err) == NULL);   /* spent */
    CHECK(puddle_identity_roll_prepare(a, p0, p1, err, sizeof err) == NULL);   /* long spent */
    chain = puddle_identity_chain(a, &cl);
    CHECK(puddle_chain_verify(chain, cl, id0, &h, verr, sizeof verr) == 0 && h.seq == 2 && lines(chain) == 3);
    free(chain);
    puddle_identity_close(a);
    a = puddle_identity_open(d, err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_close(a);
    free(chain0); free(keys0);
}

static void write_file_in(const char *dir, const char *name, const char *text, size_t len)
{
    char path[800];
    snprintf(path, sizeof path, "%s/identity", dir);
    mkdir(dir, 0700);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/identity/%s", dir, name);
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}

/* the process dies at each point of a roll: the next start finishes it, or refuses without touching anything */
static void test_interrupted_roll(void)
{
    char err[700], p0[PUDDLE_PHRASE_LEN + 1], p1[PUDDLE_PHRASE_LEN + 1], p2[PUDDLE_PHRASE_LEN + 1];
    char *d = dir_for("crash-src");
    puddle_identity *a = create_ok(d, p0, err, sizeof err);
    size_t chain_old_len, keys_old_len, chain_new_len, keys_new_len;
    char *chain_old = puddle_identity_chain(a, &chain_old_len);
    char *keys_old = file_text(d, "keys", &keys_old_len);
    char id[53];
    puddle_identity_id(a, id);
    puddle_roll *r = puddle_identity_roll_prepare(a, p0, p1, err, sizeof err);
    CHECK(r && puddle_identity_roll_commit(a, r, err, sizeof err) == 0);
    char *chain_new = puddle_identity_chain(a, &chain_new_len);
    char *keys_new = file_text(d, "keys", &keys_new_len);
    puddle_identity_close(a);

    /* what the journal holds: the new chain line, then the new keys file */
    const char *last = chain_new + chain_old_len;
    char journal[3000];
    int jl = snprintf(journal, sizeof journal, "%s%s", last, keys_new);
    puddle_phrase_keys k1;
    CHECK(puddle_identity_keys_from_phrase(p1, &k1) == 0);

    struct { const char *name; const char *chain; size_t clen; const char *keys; size_t klen; } states[] = {
        { "s1-journal-only",       chain_old, chain_old_len, keys_old, keys_old_len },  /* died right after writing the journal */
        { "s2-chain-appended",     chain_new, chain_new_len, keys_old, keys_old_len },  /* chain done, keys not yet */
        { "s3-all-but-the-delete", chain_new, chain_new_len, keys_new, keys_new_len },  /* only the journal is left over */
    };
    for (size_t i = 0; i < 3; i++) {
        char *dd = dir_for(states[i].name);
        write_file_in(dd, "chain.jsonl", states[i].chain, states[i].clen);
        write_file_in(dd, "keys", states[i].keys, states[i].klen);
        write_file_in(dd, "roll.pending", journal, (size_t)jl);
        puddle_identity *x = puddle_identity_open(dd, err, sizeof err);
        if (!x) fprintf(stderr, "%s: %s\n", states[i].name, err);
        CHECK(x != NULL);
        if (!x) continue;
        char xid[53];
        unsigned char sp[32];
        uint64_t seq;
        puddle_identity_id(x, xid);
        puddle_identity_public(x, sp, NULL, &seq);
        CHECK(strcmp(xid, id) == 0 && seq == 1 && memcmp(sp, k1.sign_pub, 32) == 0);
        size_t cl, kl;
        char *c = puddle_identity_chain(x, &cl);
        char *k = file_text(dd, "keys", &kl);
        CHECK(cl == chain_new_len && memcmp(c, chain_new, cl) == 0 && kl == keys_new_len && memcmp(k, keys_new, kl) == 0);
        free(c); free(k);
        char jp[900];
        snprintf(jp, sizeof jp, "%s/identity/roll.pending", dd);
        CHECK(access(jp, F_OK) != 0);
        /* and it is a working identity: the new phrase rolls again */
        puddle_roll *r2 = puddle_identity_roll_prepare(x, p1, p2, err, sizeof err);
        CHECK(r2 && puddle_identity_roll_commit(x, r2, err, sizeof err) == 0);
        puddle_identity_close(x);
    }

    /* a journal that doesn't belong is refused, and nothing is changed */
    char py0[PUDDLE_PHRASE_LEN + 1], py1[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *y = create_ok(dir_for("crash-other"), py0, err, sizeof err);
    size_t yo;
    char *y_old = puddle_identity_chain(y, &yo);
    puddle_roll *ry = puddle_identity_roll_prepare(y, py0, py1, err, sizeof err);
    puddle_identity_roll_commit(y, ry, err, sizeof err);
    size_t yn, yk;
    char *y_new = puddle_identity_chain(y, &yn);
    char *y_keys = file_text(dir_for("crash-other"), "keys", &yk);
    puddle_identity_close(y);
    char foreign[3000];
    int fl = snprintf(foreign, sizeof foreign, "%s%s", y_new + yo, y_keys);

    char *dd = dir_for("bad-foreign");
    write_file_in(dd, "chain.jsonl", chain_old, chain_old_len);
    write_file_in(dd, "keys", keys_old, keys_old_len);
    write_file_in(dd, "roll.pending", foreign, (size_t)fl);
    CHECK(puddle_identity_open(dd, err, sizeof err) == NULL && strstr(err, "does not follow"));
    size_t cl, kl;
    char *c = file_text(dd, "chain.jsonl", &cl);
    char *k = file_text(dd, "keys", &kl);
    CHECK(cl == chain_old_len && memcmp(c, chain_old, cl) == 0 && kl == keys_old_len && memcmp(k, keys_old, kl) == 0);
    free(c); free(k);

    /* this identity's own record but someone else's keys, or garbage, or half a file */
    dd = dir_for("bad-keys");
    char mixed[3000];
    int ml = snprintf(mixed, sizeof mixed, "%s%s", last, y_keys);
    write_file_in(dd, "chain.jsonl", chain_old, chain_old_len);
    write_file_in(dd, "keys", keys_old, keys_old_len);
    write_file_in(dd, "roll.pending", mixed, (size_t)ml);
    CHECK(puddle_identity_open(dd, err, sizeof err) == NULL && strstr(err, "does not describe a valid roll"));
    dd = dir_for("bad-garbage");
    write_file_in(dd, "chain.jsonl", chain_old, chain_old_len);
    write_file_in(dd, "keys", keys_old, keys_old_len);
    write_file_in(dd, "roll.pending", "garbage", 7);
    CHECK(puddle_identity_open(dd, err, sizeof err) == NULL);
    dd = dir_for("bad-half");
    write_file_in(dd, "chain.jsonl", chain_old, chain_old_len);
    write_file_in(dd, "keys", keys_old, keys_old_len);
    write_file_in(dd, "roll.pending", journal, (size_t)jl / 2);
    CHECK(puddle_identity_open(dd, err, sizeof err) == NULL);
    c = file_text(dd, "keys", &kl);
    CHECK(kl == keys_old_len && memcmp(c, keys_old, kl) == 0); /* the old keys are still there */
    free(c);

    free(chain_old); free(keys_old); free(chain_new); free(keys_new); free(y_old); free(y_new); free(y_keys);
}



/* ============================ restore ============================ */

static void rm_dir(const char *path) { nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

/* a copy of `chain` with every line ending changed and a blank line and some padding added */
static char *messy(const char *chain)
{
    size_t n = strlen(chain);
    char *out = malloc(n * 2 + 64);
    size_t o = 0;
    o += (size_t)sprintf(out + o, "\r\n  \r\n");           /* leading blanks */
    for (size_t i = 0; i < n; i++) {
        if (chain[i] == '\n') { out[o++] = '\r'; out[o++] = '\n'; out[o++] = '\r'; out[o++] = '\n'; }
        else out[o++] = chain[i];
    }
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) o--; /* and no final newline */
    out[o] = '\0';
    return out;
}

static void test_restore(void)
{
    char err[900], p0[PUDDLE_PHRASE_LEN + 1], p1[PUDDLE_PHRASE_LEN + 1], p2[PUDDLE_PHRASE_LEN + 1], p3[PUDDLE_PHRASE_LEN + 1];
    char d[600], d_identity[700];
    snprintf(d, sizeof d, "%s", dir_for("rst")); /* dir_for() reuses one buffer, so keep our own copy */
    snprintf(d_identity, sizeof d_identity, "%s/identity", d);

    /* an identity that has changed its keys twice */
    puddle_identity *a = create_ok(d, p0, err, sizeof err);
    char id[53];
    puddle_identity_id(a, id);
    size_t chain0_len;
    char *chain0 = puddle_identity_chain(a, &chain0_len);
    puddle_roll *r = puddle_identity_roll_prepare(a, p0, p1, err, sizeof err);
    CHECK(r && puddle_identity_roll_commit(a, r, err, sizeof err) == 0);
    r = puddle_identity_roll_prepare(a, p1, p2, err, sizeof err);
    CHECK(r && puddle_identity_roll_commit(a, r, err, sizeof err) == 0);
    size_t chain_len, keys_len;
    char *chain = puddle_identity_chain(a, &chain_len);
    char *keys = file_text(d, "keys", &keys_len);
    unsigned char sp[32], ep[32];
    uint64_t seq;
    puddle_identity_public(a, sp, ep, &seq);
    CHECK(seq == 2 && lines(chain) == 3);
    puddle_identity_close(a);

    /* everything on the machine is lost */
    rm_dir(d_identity);
    CHECK(!puddle_identity_exists(d));

    /* the wrong phrases make nothing, and say why */
    char stranger[PUDDLE_PHRASE_LEN + 1], typo[PUDDLE_PHRASE_LEN + 1];
    puddle_phrase_generate(stranger);
    snprintf(typo, sizeof typo, "%s", p2);
    typo[1] = (typo[1] == '0') ? '1' : '0';
    CHECK(puddle_identity_restore(d, p1, chain, chain_len, err, sizeof err) == NULL && strstr(err, "older recovery phrase"));
    CHECK(puddle_identity_restore(d, p0, chain, chain_len, err, sizeof err) == NULL && strstr(err, "older recovery phrase"));
    CHECK(puddle_identity_restore(d, stranger, chain, chain_len, err, sizeof err) == NULL && strstr(err, "doesn't belong to this chain"));
    CHECK(puddle_identity_restore(d, typo, chain, chain_len, err, sizeof err) == NULL && strstr(err, "typo"));
    CHECK(puddle_identity_restore(d, "hello", chain, chain_len, err, sizeof err) == NULL && strstr(err, "doesn't look like"));
    CHECK(puddle_identity_restore(d, "", chain, chain_len, err, sizeof err) == NULL);
    CHECK(puddle_identity_restore(d, NULL, chain, chain_len, err, sizeof err) == NULL);
    CHECK(!puddle_identity_exists(d)); /* none of that made anything */

    /* the wrong chains */
    CHECK(puddle_identity_restore(d, p2, "", 0, err, sizeof err) == NULL && strstr(err, "not usable"));
    CHECK(puddle_identity_restore(d, p2, NULL, 0, err, sizeof err) == NULL);
    CHECK(puddle_identity_restore(d, p2, "garbage\n", 8, err, sizeof err) == NULL && strstr(err, "not usable"));
    char t[6000];
    snprintf(t, sizeof t, "%s", chain);
    char *ts = strstr(strchr(t, '\n') + 1, "\"ts\":");             /* altered after signing */
    ts[5] = (ts[5] == '1') ? '2' : '1';
    CHECK(puddle_identity_restore(d, p2, t, strlen(t), err, sizeof err) == NULL && strstr(err, "not usable"));
    /* a copy that is behind (the newest record missing): the phrase can't be matched to it, and nothing can fix that */
    const char *second_nl = strchr(strchr(chain, '\n') + 1, '\n');
    CHECK(puddle_identity_restore(d, p2, chain, (size_t)(second_nl + 1 - chain), err, sizeof err) == NULL &&
          strstr(err, "out of date"));
    /* another identity's chain */
    char po[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *o = create_ok(dir_for("rst-other"), po, err, sizeof err);
    size_t ol;
    char *ochain = puddle_identity_chain(o, &ol);
    puddle_identity_close(o);
    CHECK(puddle_identity_restore(d, p2, ochain, ol, err, sizeof err) == NULL && strstr(err, "doesn't belong"));
    CHECK(puddle_identity_restore(d, po, chain, chain_len, err, sizeof err) == NULL && strstr(err, "doesn't belong"));
    free(ochain);
    CHECK(!puddle_identity_exists(d));

    /* the right phrase and chain: the very same identity comes back */
    puddle_identity *x = puddle_identity_restore(d, p2, chain, chain_len, err, sizeof err);
    if (!x) fprintf(stderr, "restore: %s\n", err);
    CHECK(x != NULL);
    if (x) {
        char xid[53];
        unsigned char xsp[32], xep[32];
        uint64_t xseq;
        puddle_identity_id(x, xid);
        puddle_identity_public(x, xsp, xep, &xseq);
        CHECK(strcmp(xid, id) == 0 && xseq == 2 && memcmp(xsp, sp, 32) == 0 && memcmp(xep, ep, 32) == 0);
        size_t cl, kl;
        char *c = puddle_identity_chain(x, &cl);
        char *k = file_text(d, "keys", &kl);
        CHECK(cl == chain_len && memcmp(c, chain, cl) == 0);          /* the chain is exactly what was given */
        CHECK(kl == keys_len && memcmp(k, keys, kl) == 0);            /* and the keys file is byte-for-byte what it was */
        free(c); free(k);
        CHECK(!puddle_identity_unconfirmed(d));                       /* nothing new was shown, so nothing to confirm */
        unsigned char sig[64];
        CHECK(puddle_identity_sign(x, "same me", 7, sig) == 0 && puddle_verify_signature(sp, "same me", 7, sig));
        puddle_identity_close(x);

        /* it is a full identity: it opens, and it can roll again with the phrase it was restored from */
        x = puddle_identity_open(d, err, sizeof err);
        CHECK(x != NULL);
        puddle_roll *rr = puddle_identity_roll_prepare(x, p2, p3, err, sizeof err);
        CHECK(rr && puddle_identity_roll_commit(x, rr, err, sizeof err) == 0);
        puddle_identity_public(x, NULL, NULL, &xseq);
        CHECK(xseq == 3);
        puddle_identity_close(x);
    }

    /* never over an identity that is there */
    CHECK(puddle_identity_restore(d, p2, chain, chain_len, err, sizeof err) == NULL && strstr(err, "already exists"));

    /* the secret is not left lying around */
    unsigned char seed2[32];
    puddle_phrase_seed(p2, seed2);
    char seed_hex[65], bare[40];
    puddle_hex_encode(seed2, 32, seed_hex);
    size_t o2 = 0;
    for (const char *q = p2; *q; q++) if (*q != '-') bare[o2++] = *q;
    bare[o2] = '\0';
    nneedles = 0; needles[nneedles++] = p2; needles[nneedles++] = bare; needles[nneedles++] = seed_hex;
    found_in_files = 0;
    nftw(d, scan_cb, 16, FTW_PHYS);
    CHECK(found_in_files == 0);

    /* a chain copy as it might arrive: other line endings, blank lines, no final newline */
    char *m = messy(chain);
    rm_dir(d_identity);
    x = puddle_identity_restore(d, p2, m, strlen(m), err, sizeof err);
    CHECK(x != NULL);
    if (x) {
        size_t cl;
        char *c = puddle_identity_chain(x, &cl);
        CHECK(cl == chain_len && memcmp(c, chain, cl) == 0);          /* tidied back to the canonical form */
        free(c);
        puddle_identity_close(x);
    }
    free(m);

    /* an identity that never changed its keys comes back too */
    char d0[600], d0_identity[700];
    snprintf(d0, sizeof d0, "%s", dir_for("rst0"));
    snprintf(d0_identity, sizeof d0_identity, "%s/identity", d0);
    char q0[PUDDLE_PHRASE_LEN + 1];
    a = create_ok(d0, q0, err, sizeof err);
    size_t c0l, k0l;
    char *c0 = puddle_identity_chain(a, &c0l);
    char *k0 = file_text(d0, "keys", &k0l);
    puddle_identity_close(a);
    rm_dir(d0_identity);
    x = puddle_identity_restore(d0, q0, c0, c0l, err, sizeof err);
    CHECK(x != NULL);
    if (x) {
        size_t kl;
        char *k = file_text(d0, "keys", &kl);
        CHECK(kl == k0l && memcmp(k, k0, kl) == 0);
        free(k);
        puddle_identity_close(x);
    }
    free(c0); free(k0);

    free(chain); free(keys); free(chain0);
}

/* two processes making an identity at once: exactly one wins, and what remains is whole */
static void test_create_race(void)
{
    char *d = dir_for("race");
    pid_t kids[2];
    for (int i = 0; i < 2; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            char err[300], phrase[PUDDLE_PHRASE_LEN + 1];
            puddle_identity *id = puddle_identity_create(d, phrase, err, sizeof err);
            if (id) puddle_identity_close(id);
            _exit(id ? 0 : 1);
        }
    }
    int wins = 0;
    for (int i = 0; i < 2; i++) {
        int st = 0;
        waitpid(kids[i], &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) wins++;
    }
    CHECK(wins == 1);
    char err[300];
    puddle_identity_confirm(d);
    puddle_identity *id = puddle_identity_open(d, err, sizeof err);
    CHECK(id != NULL);
    puddle_identity_close(id);
    DIR *dp = opendir(d);
    struct dirent *e;
    int stray = 0;
    while ((e = readdir(dp))) if (strncmp(e->d_name, "identity.new", 12) == 0) stray++;
    closedir(dp);
    CHECK(stray == 0); /* the loser cleaned up */
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-identity-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;
    test_known_answer();
    test_create_and_reopen();
    test_chain_verify();
    test_damaged_and_old();
    test_unconfirmed();
    test_forged_inception();
    test_rotation_rules();
    test_roll_flow();
    test_interrupted_roll();
    test_restore();
    test_create_race();
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
