#include "core/identity.h"

#include "platform/clock.h"
#include "core/config.h"
#include "base/encoding.h"
#include "platform/fsutil.h"
#include "platform/process.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/log.h"

#include <errno.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>

#define MAX_CHAIN_BYTES (4u << 20)
#define MAX_CHAIN_RECORDS 10000
#define KEYS_HEADER "puddle-keys 2"
#define KEY_SALT "puddle-keys-v1"

typedef struct {
    unsigned char priv[32];
    unsigned char pub[32];
} keypair;

/* everything one phrase makes */
typedef struct {
    keypair sign, enc, roll;
} generation;

struct puddle_identity {
    char dir[PUDDLE_PATH_MAX];
    char keys_path[PUDDLE_PATH_MAX];
    char chain_path[PUDDLE_PATH_MAX];
    std::mutex mu;
    keypair sign, enc;
    char id[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;
    char head_hash[65];
    char next[65]; /* what the head record commits to: SHA-256 of the next roll key */
};

/* a roll that has been prepared and is waiting for the owner to confirm the new phrase */
struct puddle_roll {
    char base_head[65];   /* the chain head it was prepared against */
    keypair old_roll;     /* the current generation's roll key, from the typed phrase */
    keypair new_sign, new_enc;
    char next_commit[65]; /* SHA-256 of the new generation's roll key */
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void phrase_problem(int parse_result, char *err, size_t errlen);

static int64_t now_ms(void)
{
    return puddle_wall_ms();
}

/* ------------------------------------------------------------------ crypto */

/* the key pair for a private half; `type` is EVP_PKEY_ED25519 or EVP_PKEY_X25519 */
static int keypair_from_priv(int type, const unsigned char priv[32], keypair *kp)
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(type, NULL, priv, 32);
    if (!pk)
        return -1;
    size_t qlen = 32;
    int ok = EVP_PKEY_get_raw_public_key(pk, kp->pub, &qlen) == 1 && qlen == 32;
    EVP_PKEY_free(pk);
    memcpy(kp->priv, priv, 32);
    return ok ? 0 : -1;
}

/* One private key per label, each from the phrase's seed. The labels are part of the format: change one
 * and every existing identity stops matching its phrase (tests/identity_test.c pins them). */
static int derive_one(const unsigned char seed[32], const char *label, int type, keypair *kp)
{
    unsigned char priv[32];
    if (puddle_hkdf_sha256(seed, 32, KEY_SALT, label, priv, 32) != 0)
        return -1;
    int rc = keypair_from_priv(type, priv, kp);
    memset(priv, 0, sizeof priv);
    return rc;
}

static int derive_generation(const char *phrase, generation *g)
{
    unsigned char seed[32];
    if (puddle_phrase_seed(phrase, seed) != PUDDLE_PHRASE_OK)
        return -1;
    int rc = derive_one(seed, "signing", EVP_PKEY_ED25519, &g->sign) ||
             derive_one(seed, "encryption", EVP_PKEY_X25519, &g->enc) ||
             derive_one(seed, "roll", EVP_PKEY_ED25519, &g->roll);
    memset(seed, 0, sizeof seed);
    return rc;
}

static int sign_with(const keypair *kp, const void *data, size_t len, unsigned char sig[64])
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, kp->priv, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t siglen = 64;
    int ok = pk && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestSign(ctx, sig, &siglen, (const unsigned char *)data, len) == 1 && siglen == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

int puddle_verify_signature(const unsigned char sign_pub[32], const void *data, size_t len,
                            const unsigned char sig[64])
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, sign_pub, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = pk && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestVerify(ctx, sig, 64, (const unsigned char *)data, len) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

void puddle_key_id(const unsigned char sign_pub[32], char out[65])
{
    puddle_sha256_hex(sign_pub, 32, out);
}

/* The user id binds the first signing key AND the commitment to the first roll key: SHA-256 of the key
 * followed by SHA-256 of the roll key. A thief who holds only the daily keys can sign a different first
 * record, but not one whose id comes out the same, because his roll key hashes to something else. */
static void user_id_of(const unsigned char first_sign_pub[32], const unsigned char first_roll_commit[32],
                       char out[PUDDLE_USER_ID_LEN + 1])
{
    unsigned char both[64], md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    memcpy(both, first_sign_pub, 32);
    memcpy(both + 32, first_roll_commit, 32);
    EVP_Digest(both, sizeof both, md, &mdlen, EVP_sha256(), NULL);
    puddle_base32_encode(md, mdlen, out, PUDDLE_USER_ID_LEN + 1);
}

static void user_id_from_roll_pub(const unsigned char first_sign_pub[32], const unsigned char first_roll_pub[32],
                                  char out[PUDDLE_USER_ID_LEN + 1])
{
    unsigned char commit[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_Digest(first_roll_pub, 32, commit, &n, EVP_sha256(), NULL);
    user_id_of(first_sign_pub, commit, out);
}

int puddle_identity_keys_from_phrase(const char *phrase, puddle_phrase_keys *out)
{
    generation g;
    if (derive_generation(phrase, &g) != 0)
        return -1;
    memcpy(out->sign_pub, g.sign.pub, 32);
    memcpy(out->enc_pub, g.enc.pub, 32);
    memcpy(out->roll_pub, g.roll.pub, 32);
    memset(&g, 0, sizeof g);
    return 0;
}

/* ------------------------------------------------------------------ records */

/* The first chain line: {"record":{...},"sig":"<b64>"}\n, the signature over the exact record bytes.
 * `next` commits to the roll key. Returns the line length, or 0. */
static size_t build_inception(char *out, size_t cap, const generation *g, char head_hash_out[65])
{
    char sp[64], ep[64], next[65];
    puddle_base64_encode(g->sign.pub, 32, sp, sizeof sp);
    puddle_base64_encode(g->enc.pub, 32, ep, sizeof ep);
    puddle_sha256_hex(g->roll.pub, 32, next);

    char rec[400];
    int n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.inception\",\"seq\":0,\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":%lld}",
                     sp, ep, next, (long long)now_ms());
    if (n <= 0 || (size_t)n >= sizeof rec)
        return 0;
    unsigned char sig[64];
    char sig64[100];
    if (sign_with(&g->sign, rec, (size_t)n, sig) != 0 || !puddle_base64_encode(sig, 64, sig64, sizeof sig64))
        return 0;
    puddle_sha256_hex(rec, (size_t)n, head_hash_out);
    int w = snprintf(out, cap, "{\"record\":%s,\"sig\":\"%s\"}\n", rec, sig64);
    return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}

/* A rotation record: {"record":{...},"sig":..,"sig_roll":..,"sig_prev":..}\n. All three signatures cover
 * the exact record bytes. `roll` reveals the roll key the previous record committed to. */
static size_t build_rotate(char *out, size_t cap, uint64_t seq, const char *prev_hash, const keypair *old_sign,
                           const keypair *old_roll, const keypair *new_sign, const keypair *new_enc,
                           const char next_commit[65], char head_hash_out[65])
{
    char sp[64], ep[64], rp[64];
    puddle_base64_encode(new_sign->pub, 32, sp, sizeof sp);
    puddle_base64_encode(new_enc->pub, 32, ep, sizeof ep);
    puddle_base64_encode(old_roll->pub, 32, rp, sizeof rp);

    char rec[700];
    int n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.rotate\",\"seq\":%llu,\"prev\":\"%s\",\"signing\":\"%s\",\"encryption\":\"%s\",\"roll\":\"%s\",\"next\":\"%s\",\"ts\":%lld}",
                     (unsigned long long)seq, prev_hash, sp, ep, rp, next_commit, (long long)now_ms());
    if (n <= 0 || (size_t)n >= sizeof rec)
        return 0;

    unsigned char s1[64], s2[64], s3[64];
    char b1[100], b2[100], b3[100];
    if (sign_with(new_sign, rec, (size_t)n, s1) != 0 || sign_with(old_roll, rec, (size_t)n, s2) != 0 ||
        sign_with(old_sign, rec, (size_t)n, s3) != 0 || !puddle_base64_encode(s1, 64, b1, sizeof b1) ||
        !puddle_base64_encode(s2, 64, b2, sizeof b2) || !puddle_base64_encode(s3, 64, b3, sizeof b3))
        return 0;
    puddle_sha256_hex(rec, (size_t)n, head_hash_out);
    int w = snprintf(out, cap, "{\"record\":%s,\"sig\":\"%s\",\"sig_roll\":\"%s\",\"sig_prev\":\"%s\"}\n", rec, b1, b2, b3);
    return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}

static int append_line(const char *path, const char *dir, const char *line, size_t len)
{
    int fd = puddle_fd_open(path, PUDDLE_F_WRITE | PUDDLE_F_APPEND | PUDDLE_F_CREATE);
    if (fd < 0)
        return -1;
    int64_t before = puddle_fd_end(fd);
    if (puddle_fd_write_all(fd, line, len) != 0) {
        if (before >= 0 && puddle_fd_truncate(fd, before) != 0) {} /* don't leave half a record behind */
        puddle_fd_close(fd);
        return -1;
    }
    int rc = puddle_fsync_fd(fd);
    puddle_fd_close(fd);
    if (rc == 0)
        puddle_fsync_dir(dir);
    return rc;
}

/* ------------------------------------------------------------- chain verify */

static int cfail(char *err, size_t errlen, size_t line, const char *what)
{
    seterr(err, errlen, "identity chain record %zu: %s", line, what);
    return -1;
}

static int get_key(const char *rec, size_t len, const char *name, unsigned char out[32])
{
    puddle_span v;
    char text[64];
    size_t n = 0;
    unsigned char tmp[40];
    if (puddle_json_member(rec, len, name, &v) != 0 || puddle_json_string(v, text, sizeof text) != 0 ||
        puddle_base64_decode(text, tmp, sizeof tmp, &n) != 0 || n != 32)
        return -1;
    memcpy(out, tmp, 32);
    return 0;
}

static int get_hex64(const char *rec, size_t len, const char *name, char out[65])
{
    puddle_span v;
    unsigned char raw[32];
    if (puddle_json_member(rec, len, name, &v) != 0 || puddle_json_string(v, out, 65) != 0 ||
        puddle_hex_decode(out, raw, 32) != 0)
        return -1;
    return 0;
}

static int get_sig(const char *line, size_t len, const char *name, unsigned char out[64])
{
    puddle_span v;
    char text[100];
    size_t n = 0;
    unsigned char tmp[72];
    if (puddle_json_member(line, len, name, &v) != 0 || puddle_json_string(v, text, sizeof text) != 0 ||
        puddle_base64_decode(text, tmp, sizeof tmp, &n) != 0 || n != 64)
        return -1;
    memcpy(out, tmp, 64);
    return 0;
}

int puddle_chain_verify(const char *text, size_t len, const char *expected_id, puddle_chain_head *out,
                        char *err, size_t errlen)
{
    if (len == 0 || len > MAX_CHAIN_BYTES)
        return cfail(err, errlen, 0, "empty or too large");

    puddle_chain_head h;
    memset(&h, 0, sizeof h);
    size_t count = 0, pos = 0;

    while (pos < len) {
        const char *line = text + pos;
        const char *nl = (const char *)memchr(line, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - line) : len - pos;
        pos += ll + (nl ? 1 : 0);
        if (ll == 0)
            continue;
        if (++count > MAX_CHAIN_RECORDS)
            return cfail(err, errlen, count, "too many records");

        puddle_span rv;
        char type[32];
        unsigned char sig[64];
        int64_t seq;
        if (!puddle_json_valid_object(line, ll) || puddle_json_member(line, ll, "record", &rv) != 0 ||
            rv.len == 0 || rv.p[0] != '{' || get_sig(line, ll, "sig", sig) != 0)
            return cfail(err, errlen, count, "malformed");

        const char *rec = rv.p;
        size_t rl = rv.len;
        puddle_span t;
        if (puddle_json_member(rec, rl, "type", &t) != 0 || puddle_json_string(t, type, sizeof type) != 0 ||
            puddle_json_member(rec, rl, "seq", &t) != 0 || puddle_json_int64(t, &seq) != 0)
            return cfail(err, errlen, count, "missing type or seq");

        unsigned char sp[32], ep[32];
        char next[65];
        if (get_key(rec, rl, "signing", sp) != 0 || get_key(rec, rl, "encryption", ep) != 0 ||
            get_hex64(rec, rl, "next", next) != 0)
            return cfail(err, errlen, count, "bad keys or commitment");

        if (count == 1) {
            if (strcmp(type, "identity.inception") != 0 || seq != 0)
                return cfail(err, errlen, count, "the chain must start with an inception record");
            unsigned char commit_raw[32];
            puddle_hex_decode(next, commit_raw, 32); /* already checked to be 64 hex characters */
            user_id_of(sp, commit_raw, h.id);
            if (expected_id && strcmp(expected_id, h.id) != 0)
                return cfail(err, errlen, count, "this chain belongs to a different id");
            if (!puddle_verify_signature(sp, rec, rl, sig))
                return cfail(err, errlen, count, "bad signature");
        } else {
            char prev[65], roll_hash[65];
            unsigned char roll_pub[32], sig_roll[64], sig_prev[64];
            if (strcmp(type, "identity.rotate") != 0 || seq < 0 || (uint64_t)seq != h.seq + 1)
                return cfail(err, errlen, count, "out of order");
            if (get_hex64(rec, rl, "prev", prev) != 0 || strcmp(prev, h.head_hash) != 0)
                return cfail(err, errlen, count, "does not follow the previous record");
            if (get_key(rec, rl, "roll", roll_pub) != 0 || get_sig(line, ll, "sig_roll", sig_roll) != 0 ||
                get_sig(line, ll, "sig_prev", sig_prev) != 0)
                return cfail(err, errlen, count, "malformed");
            /* the authority to change keys: the roll key the previous record committed to */
            puddle_sha256_hex(roll_pub, 32, roll_hash);
            if (strcmp(roll_hash, h.next) != 0)
                return cfail(err, errlen, count, "the roll key is not the one the previous record committed to");
            if (!puddle_verify_signature(roll_pub, rec, rl, sig_roll))
                return cfail(err, errlen, count, "bad signature from the roll key");
            if (!puddle_verify_signature(sp, rec, rl, sig))
                return cfail(err, errlen, count, "bad signature from the new key");
            if (!puddle_verify_signature(h.sign_pub, rec, rl, sig_prev))
                return cfail(err, errlen, count, "bad signature from the previous key");
        }

        h.seq = (uint64_t)seq;
        memcpy(h.sign_pub, sp, 32);
        memcpy(h.enc_pub, ep, 32);
        memcpy(h.next, next, 65);
        puddle_sha256_hex(rec, rl, h.head_hash);
    }
    if (count == 0)
        return cfail(err, errlen, 0, "empty");
    *out = h;
    return 0;
}

/* ------------------------------------------------------------- the files */

static void paths(const char *data_dir, char dir[PUDDLE_PATH_MAX])
{
    snprintf(dir, PUDDLE_PATH_MAX, "%s/identity", data_dir);
}

int puddle_identity_exists(const char *data_dir)
{
    char dir[PUDDLE_PATH_MAX];
    paths(data_dir, dir);
    return puddle_path_is_dir(dir);
}

static void marker_path(const char *dir, char out[PUDDLE_PATH_MAX])
{
    snprintf(out, PUDDLE_PATH_MAX, "%s/unconfirmed", dir);
}

int puddle_identity_unconfirmed(const char *data_dir)
{
    char dir[PUDDLE_PATH_MAX], marker[PUDDLE_PATH_MAX];
    paths(data_dir, dir);
    marker_path(dir, marker);
    return puddle_path_exists(marker);
}

int puddle_identity_confirm(const char *data_dir)
{
    char dir[PUDDLE_PATH_MAX], marker[PUDDLE_PATH_MAX];
    if (!puddle_identity_exists(data_dir))
        return -1;
    paths(data_dir, dir);
    marker_path(dir, marker);
    if (puddle_remove_file(marker) != 0)
        return -1;
    puddle_fsync_dir(dir);
    return 0;
}

/* the daily keys only, never the phrase or the roll key */
static void remove_tree(const char *dir);

int puddle_identity_discard_unconfirmed(const char *data_dir, char *err, size_t errlen)
{
    char dir[PUDDLE_PATH_MAX];
    if (!puddle_identity_unconfirmed(data_dir))
        return 1; /* nothing unconfirmed: and a confirmed identity is never touched */
    paths(data_dir, dir);
    remove_tree(dir);
    if (puddle_identity_exists(data_dir)) {
        seterr(err, errlen, "cannot remove the unfinished identity in %s", dir);
        return -1;
    }
    puddle_fsync_dir(data_dir);
    return 0;
}

/* the keys file as text; returns its length, or -1 if it does not fit */
static int format_keys(char *out, size_t cap, const char *id, const keypair *sign, const keypair *enc)
{
    char h1[65], h2[65];
    puddle_hex_encode(sign->priv, 32, h1);
    puddle_hex_encode(enc->priv, 32, h2);
    int n = snprintf(out, cap, KEYS_HEADER "\nid %s\nsigning %s\nencryption %s\n", id, h1, h2);
    memset(h1, 0, sizeof h1);
    memset(h2, 0, sizeof h2);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* the daily keys only, never the phrase or the roll key */
static int write_keys(const char *dir, const char *path, const char *id, const keypair *sign, const keypair *enc)
{
    char buf[400];
    int n = format_keys(buf, sizeof buf, id, sign, enc);
    int rc = n < 0 ? -1 : puddle_write_file_atomic(dir, path, buf, (size_t)n);
    memset(buf, 0, sizeof buf);
    return rc;
}

/* Parse keys-file text. 0 ok, 1 an older development format, -1 damaged. */
static int parse_keys_text(const char *text, char file_id[PUDDLE_USER_ID_LEN + 1], keypair *sign, keypair *enc)
{
    char name[32], hex[80];
    unsigned char s[32], entry[32];
    int have = 0, header = 0, old = 0;
    file_id[0] = '\0';
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (ll >= sizeof line)
            return -1;
        memcpy(line, p, ll);
        line[ll] = '\0';
        p += ll + (nl ? 1 : 0);
        if (strncmp(line, KEYS_HEADER, strlen(KEYS_HEADER)) == 0) {
            header = 1;
        } else if (strncmp(line, "puddle-keys 1", 13) == 0) {
            old = 1;
        } else if (sscanf(line, "%31s %79s", name, hex) == 2) {
            if (strcmp(name, "id") == 0 && strlen(hex) == PUDDLE_USER_ID_LEN)
                memcpy(file_id, hex, PUDDLE_USER_ID_LEN + 1);
            else if (strcmp(name, "signing") == 0 && puddle_hex_decode(hex, s, 32) == 0)
                have |= 1;
            else if (strcmp(name, "encryption") == 0 && puddle_hex_decode(hex, entry, 32) == 0)
                have |= 2;
        }
    }
    memset(hex, 0, sizeof hex);
    int rc = 0;
    if (old)
        rc = 1;
    else if (!header || have != 3 || !file_id[0] || keypair_from_priv(EVP_PKEY_ED25519, s, sign) != 0 ||
             keypair_from_priv(EVP_PKEY_X25519, entry, enc) != 0)
        rc = -1;
    memset(s, 0, sizeof s);
    memset(entry, 0, sizeof entry);
    return rc;
}

/* 0 loaded, 1 file missing, -1 bad (message set) */
static int read_keys(puddle_identity *id, char file_id[PUDDLE_USER_ID_LEN + 1], char *err, size_t errlen)
{
    FILE *f = fopen(id->keys_path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 1;
        seterr(err, errlen, "cannot read %s: %s", id->keys_path, strerror(errno));
        return -1;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    int rc = parse_keys_text(buf, file_id, &id->sign, &id->enc);
    memset(buf, 0, sizeof buf);
    if (rc == 1) {
        seterr(err, errlen, "%s was made by an older development version that had no recovery phrase; "
               "move %s aside and make a new identity with `puddled --init`", id->keys_path, id->dir);
        return -1;
    }
    if (rc != 0) {
        seterr(err, errlen, "%s is damaged; restore it from a backup", id->keys_path);
        return -1;
    }
    return 0;
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)malloc(MAX_CHAIN_BYTES + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, MAX_CHAIN_BYTES + 1, f);
    fclose(f);
    if (n > MAX_CHAIN_BYTES) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static puddle_identity *blank(const char *data_dir)
{
    puddle_identity *id = new (std::nothrow) puddle_identity();
    if (!id)
        return NULL;
    paths(data_dir, id->dir);
    snprintf(id->keys_path, sizeof id->keys_path, "%s/keys", id->dir);
    snprintf(id->chain_path, sizeof id->chain_path, "%s/chain.jsonl", id->dir);
    return id;
}

/* ------------------------------------------------------------------- rolling */

/* The roll journal: the signed chain line, then the new keys file, written as one file before either real
 * file is touched. If the process dies part way, finishing from it is always possible and always safe. */
static void journal_path(const puddle_identity *id, char out[PUDDLE_PATH_MAX])
{
    snprintf(out, PUDDLE_PATH_MAX, "%s/roll.pending", id->dir);
}

/* Finish an interrupted roll, if there is one (`announce`: say so in the log). 0 nothing to do or finished,
 * -1 refused (message set). */
static int apply_pending(puddle_identity *id, int announce, char *err, size_t errlen)
{
    char jpath[PUDDLE_PATH_MAX];
    journal_path(id, jpath);
    size_t jlen = 0;
    char *journal = read_all(jpath, &jlen);
    if (!journal)
        return 0;

    int rc = -1;
    char *chain = NULL;
    const char *nl = strchr(journal, '\n');
    if (!nl || nl == journal) {
        seterr(err, errlen, "%s is damaged; the roll it records can't be finished", jpath);
        goto out;
    }
    size_t line_len; line_len = (size_t)(nl - journal) + 1; /* includes the newline */
    const char *keys_text; keys_text = nl + 1;

    /* what the record says, and that the keys it travels with are really its keys */
    puddle_span rv;
    unsigned char rsign[32], renc[32];
    char prev[65], rec_hash[65], fid[PUDDLE_USER_ID_LEN + 1];
    keypair ksign, kenc;
    if (puddle_json_member(journal, line_len - 1, "record", &rv) != 0 || get_hex64(rv.p, rv.len, "prev", prev) != 0 ||
        get_key(rv.p, rv.len, "signing", rsign) != 0 || get_key(rv.p, rv.len, "encryption", renc) != 0 ||
        parse_keys_text(keys_text, fid, &ksign, &kenc) != 0 || memcmp(ksign.pub, rsign, 32) != 0 ||
        memcmp(kenc.pub, renc, 32) != 0) {
        seterr(err, errlen, "%s does not describe a valid roll; remove it only if you are sure no roll was in progress",
               jpath);
        goto out;
    }
    puddle_sha256_hex(rv.p, rv.len, rec_hash);

    size_t clen; clen = 0;
    chain = read_all(id->chain_path, &clen);
    puddle_chain_head h;
    char why[200];
    if (!chain || clen == 0 || puddle_chain_verify(chain, clen, NULL, &h, why, sizeof why) != 0) {
        seterr(err, errlen, "%s: %s", id->chain_path, chain && clen ? why : "missing or empty");
        goto out;
    }
    if (strcmp(h.head_hash, rec_hash) == 0) {
        /* the record is already in the chain */
    } else if (strcmp(prev, h.head_hash) == 0) {
        char *both = (char *)malloc(clen + line_len + 1);
        if (!both)
            goto out;
        memcpy(both, chain, clen);
        memcpy(both + clen, journal, line_len);
        both[clen + line_len] = '\0';
        puddle_chain_head after;
        int ok = puddle_chain_verify(both, clen + line_len, h.id, &after, why, sizeof why) == 0;
        free(both);
        if (!ok || append_line(id->chain_path, id->dir, journal, line_len) != 0) {
            seterr(err, errlen, "cannot finish the interrupted roll: %s", ok ? strerror(errno) : why);
            goto out;
        }
    } else {
        seterr(err, errlen, "%s does not follow this identity's chain; remove it only if you are sure no roll was in "
               "progress", jpath);
        goto out;
    }

    if (puddle_write_file_atomic(id->dir, id->keys_path, keys_text, strlen(keys_text)) != 0) {
        seterr(err, errlen, "cannot write %s: %s", id->keys_path, strerror(errno));
        goto out;
    }
    puddle_remove_file(jpath);
    puddle_fsync_dir(id->dir);
    if (announce)
        puddle_log(PUDDLE_LOG_WARN, "finished an interrupted key roll");
    rc = 0;
out:
    memset(journal, 0, jlen);
    free(journal);
    free(chain);
    memset(&ksign, 0, sizeof ksign);
    memset(&kenc, 0, sizeof kenc);
    return rc;
}

puddle_roll *puddle_identity_roll_prepare(puddle_identity *id, const char *current_phrase,
                                          char new_phrase_out[PUDDLE_PHRASE_LEN + 1], char *err, size_t errlen)
{
    char canon[PUDDLE_PHRASE_LEN + 1];
    int pr = puddle_phrase_parse(current_phrase, canon);
    if (pr != PUDDLE_PHRASE_OK) {
        phrase_problem(pr, err, errlen);
        return NULL;
    }

    generation cur, next;
    memset(&cur, 0, sizeof cur);
    memset(&next, 0, sizeof next);
    puddle_roll *roll = NULL;
    char new_phrase[PUDDLE_PHRASE_LEN + 1];
    memset(new_phrase, 0, sizeof new_phrase);
    if (derive_generation(canon, &cur) != 0) {
        seterr(err, errlen, "cannot derive keys from that phrase");
        goto out;
    }

    /* It must be the phrase of the generation in use: its roll key is what the chain committed to. An older
     * phrase, whose roll key was spent by a later roll, fails here, which is what makes a leak of it harmless. */
    id->mu.lock();
    char hash[65];
    puddle_sha256_hex(cur.roll.pub, 32, hash);
    int current; current = strcmp(hash, id->next) == 0 && memcmp(cur.sign.pub, id->sign.pub, 32) == 0;
    char base[65];
    memcpy(base, id->head_hash, 65);
    id->mu.unlock();
    if (!current) {
        seterr(err, errlen, "that is not this identity's current recovery phrase (if you have changed your keys "
               "since you wrote it down, use the newer phrase)");
        goto out;
    }

    if (puddle_phrase_generate(new_phrase) != 0 || derive_generation(new_phrase, &next) != 0) {
        seterr(err, errlen, "cannot make a new recovery phrase");
        goto out;
    }
    roll = (puddle_roll *)calloc(1, sizeof *roll);
    if (!roll) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    memcpy(roll->base_head, base, 65);
    roll->old_roll = cur.roll;
    roll->new_sign = next.sign;
    roll->new_enc = next.enc;
    puddle_sha256_hex(next.roll.pub, 32, roll->next_commit); /* the new roll key itself is not kept */
    memcpy(new_phrase_out, new_phrase, sizeof new_phrase);
out:
    memset(&cur, 0, sizeof cur);
    memset(&next, 0, sizeof next);
    memset(new_phrase, 0, sizeof new_phrase);
    memset(canon, 0, sizeof canon);
    return roll;
}

void puddle_roll_abandon(puddle_roll *roll)
{
    if (!roll)
        return;
    memset(roll, 0, sizeof *roll);
    free(roll);
}

int puddle_identity_roll_commit(puddle_identity *id, puddle_roll *roll, char *err, size_t errlen)
{
    int rc = -1;
    char line[1400], keys[400], *journal = NULL;
    char head[65];
    id->mu.lock();

    if (strcmp(roll->base_head, id->head_hash) != 0) {
        seterr(err, errlen, "this identity changed since the roll was prepared; start again");
        goto out;
    }
    size_t ll; ll = build_rotate(line, sizeof line, id->seq + 1, id->head_hash, &id->sign, &roll->old_roll,
                             &roll->new_sign, &roll->new_enc, roll->next_commit, head);
    int kl; kl = format_keys(keys, sizeof keys, id->id, &roll->new_sign, &roll->new_enc);
    if (!ll || kl < 0) {
        seterr(err, errlen, "cannot build the new record");
        goto out;
    }

    /* the verdict on the whole thing before anything is touched: would the chain accept this record? */
    char *chain; chain = NULL;
    size_t clen; clen = 0;
    chain = read_all(id->chain_path, &clen);
    char *both; both = (char *)(chain ? malloc(clen + ll + 1) : NULL);
    int ok; ok = 0;
    if (both) {
        memcpy(both, chain, clen);
        memcpy(both + clen, line, ll);
        puddle_chain_head after;
        char why[200];
        ok = puddle_chain_verify(both, clen + ll, id->id, &after, why, sizeof why) == 0;
        if (!ok)
            seterr(err, errlen, "the new record would not verify: %s", why);
    }
    free(both);
    free(chain);
    if (!ok) {
        if (!err || !err[0]) seterr(err, errlen, "cannot check the new record");
        goto out;
    }

    journal = (char *)malloc(ll + (size_t)kl + 1);
    if (!journal) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    memcpy(journal, line, ll);
    memcpy(journal + ll, keys, (size_t)kl);
    journal[ll + (size_t)kl] = '\0';
    char jpath[PUDDLE_PATH_MAX];
    journal_path(id, jpath);
    if (puddle_write_file_atomic(id->dir, jpath, journal, ll + (size_t)kl) != 0) {
        seterr(err, errlen, "cannot write %s: %s; nothing has changed", jpath, strerror(errno));
        goto out;
    }

    /* from here on the roll is decided: if anything below fails the next start finishes it */
    char why2[300];
    if (apply_pending(id, 0, why2, sizeof why2) != 0) {
        seterr(err, errlen, "the roll is recorded and will finish the next time the Puddle starts (%s)", why2);
        goto out;
    }
    id->sign = roll->new_sign;
    id->enc = roll->new_enc;
    id->seq++;
    memcpy(id->head_hash, head, 65);
    memcpy(id->next, roll->next_commit, 65);
    rc = 0;
out:
    if (journal) {
        memset(journal, 0, ll + (size_t)kl);
        free(journal);
    }
    memset(keys, 0, sizeof keys);
    id->mu.unlock();
    if (rc == 0)
        puddle_roll_abandon(roll); /* spent: wipes the old roll key and the new daily keys held for it */
    return rc;
}

/* A phrase that didn't parse, in words for the owner. */
static void phrase_problem(int parse_result, char *err, size_t errlen)
{
    if (parse_result == PUDDLE_PHRASE_BAD_CHECKSUM)
        seterr(err, errlen, "that recovery phrase has a typo in it; check each group against what you wrote down");
    else
        seterr(err, errlen, "that doesn't look like a recovery phrase: it should be eight groups of four characters");
}

/* Build <data_dir>/identity in a private directory and move it into place in one step, so a crash leaves
 * nothing half made and two processes can't both succeed. `unconfirmed` adds the marker (a new identity whose
 * phrase has not yet been acknowledged). 0 or -1 (message set). */
static int install_identity(const char *data_dir, const char *final_dir, const char *id, const keypair *sign,
                            const keypair *enc, const char *chain_text, size_t chain_len, int unconfirmed,
                            char *err, size_t errlen)
{
    char tmp[PUDDLE_PATH_MAX], keys[PUDDLE_PATH_MAX], chain[PUDDLE_PATH_MAX], marker[PUDDLE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/identity.new-%ld", data_dir, puddle_process_id());
    snprintf(keys, sizeof keys, "%s/keys", tmp);
    snprintf(chain, sizeof chain, "%s/chain.jsonl", tmp);
    remove_tree(tmp);
    if (puddle_mkdir_p(tmp) != 0) {
        seterr(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (write_keys(tmp, keys, id, sign, enc) != 0 || puddle_write_file_atomic(tmp, chain, chain_text, chain_len) != 0) {
        seterr(err, errlen, "cannot write the identity files in %s: %s", tmp, strerror(errno));
        remove_tree(tmp);
        return -1;
    }
    if (unconfirmed) {
        static const char note[] = "the recovery phrase was shown but not yet confirmed as written down\n";
        marker_path(tmp, marker);
        if (puddle_write_file_atomic(tmp, marker, note, sizeof note - 1) != 0) {
            seterr(err, errlen, "cannot write the identity files in %s: %s", tmp, strerror(errno));
            remove_tree(tmp);
            return -1;
        }
    }
    puddle_fsync_dir(tmp);
    if (puddle_rename(tmp, final_dir) != 0) {
        seterr(err, errlen, errno == ENOTEMPTY || errno == EEXIST ? "an identity already exists in %s"
               : "cannot put the identity in place in %s", data_dir);
        remove_tree(tmp);
        return -1;
    }
    puddle_fsync_dir(data_dir);
    return 0;
}

/* ------------------------------------------------------------------ create */

static void remove_tree(const char *dir)
{
    puddle_dir *d = puddle_dir_open(dir);
    if (!d)
        return;
    const char *entry;
    while ((entry = puddle_dir_next(d))) {
        char p[PUDDLE_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, entry);
        puddle_remove_file(p);
    }
    puddle_dir_close(d);
    puddle_remove_dir(dir);
}

puddle_identity *puddle_identity_create(const char *data_dir, char phrase_out[PUDDLE_PHRASE_LEN + 1],
                                        char *err, size_t errlen)
{
    generation g;
    memset(&g, 0, sizeof g);
    char phrase[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *id = NULL;

    if (puddle_identity_exists(data_dir)) {
        seterr(err, errlen, "an identity already exists in %s", data_dir);
        return NULL;
    }
    if (puddle_phrase_generate(phrase) != 0 || derive_generation(phrase, &g) != 0) {
        seterr(err, errlen, "cannot generate keys");
        goto out;
    }
    id = blank(data_dir);
    if (!id) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    id->sign = g.sign;
    id->enc = g.enc;
    user_id_from_roll_pub(g.sign.pub, g.roll.pub, id->id);
    puddle_sha256_hex(g.roll.pub, 32, id->next);

    char line[800];
    size_t ll; ll = build_inception(line, sizeof line, &g, id->head_hash);
    if (!ll) {
        seterr(err, errlen, "cannot make the first record");
        goto fail;
    }

    if (install_identity(data_dir, id->dir, id->id, &g.sign, &g.enc, line, ll, 1, err, errlen) != 0)
        goto fail;

    memcpy(phrase_out, phrase, sizeof phrase);
    goto out;
fail:
    puddle_identity_close(id);
    id = NULL;
out:
    memset(&g, 0, sizeof g);
    memset(phrase, 0, sizeof phrase);
    return id;
}

/* -------------------------------------------------------------------- restore */

/* The chain with line endings and blank lines tidied: every record on its own line ending in "\n". The
 * records themselves are not touched, so their signatures still hold. malloc'd. */
static char *normalize_chain(const char *text, size_t len, size_t *out_len)
{
    char *out = (char *)malloc(len + 2);
    if (!out)
        return NULL;
    size_t o = 0, pos = 0;
    while (pos < len) {
        const char *nl = (const char *)memchr(text + pos, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - (text + pos)) : len - pos;
        size_t keep = ll;
        while (keep && (text[pos + keep - 1] == '\r' || text[pos + keep - 1] == ' ' || text[pos + keep - 1] == '\t'))
            keep--;
        if (keep) {
            memcpy(out + o, text + pos, keep);
            o += keep;
            out[o++] = '\n';
        }
        pos += ll + (nl ? 1 : 0);
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* Does an EARLIER record of this chain announce `sign_pub`? (Then the phrase is an old one.) */
static int announced_earlier(const char *chain, size_t len, const unsigned char sign_pub[32], uint64_t head_seq)
{
    size_t pos = 0;
    uint64_t n = 0;
    while (pos < len) {
        const char *nl = (const char *)memchr(chain + pos, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - (chain + pos)) : len - pos;
        puddle_span rv;
        unsigned char k[32];
        if (n < head_seq && puddle_json_member(chain + pos, ll, "record", &rv) == 0 &&
            get_key(rv.p, rv.len, "signing", k) == 0 && memcmp(k, sign_pub, 32) == 0)
            return 1;
        n++;
        pos += ll + (nl ? 1 : 0);
    }
    return 0;
}

puddle_identity *puddle_identity_restore(const char *data_dir, const char *phrase, const char *chain_text,
                                         size_t chain_len, char *err, size_t errlen)
{
    generation g;
    memset(&g, 0, sizeof g);
    char canon[PUDDLE_PHRASE_LEN + 1], *chain = NULL;
    puddle_identity *id = NULL;

    if (puddle_identity_exists(data_dir)) {
        seterr(err, errlen, "an identity already exists in %s; it is not overwritten", data_dir);
        return NULL;
    }
    int pr = puddle_phrase_parse(phrase, canon);
    if (pr != PUDDLE_PHRASE_OK) {
        phrase_problem(pr, err, errlen);
        return NULL;
    }
    if (derive_generation(canon, &g) != 0) {
        seterr(err, errlen, "cannot derive keys from that phrase");
        goto out;
    }

    size_t clen; clen = 0;
    chain = chain_text ? normalize_chain(chain_text, chain_len, &clen) : NULL;
    puddle_chain_head h;
    char why[200];
    if (!chain || clen == 0 || puddle_chain_verify(chain, clen, NULL, &h, why, sizeof why) != 0) {
        seterr(err, errlen, "that chain is not usable: %s", chain && clen ? why : "it is empty");
        goto out;
    }

    char roll_hash[65];
    puddle_sha256_hex(g.roll.pub, 32, roll_hash);
    if (memcmp(g.sign.pub, h.sign_pub, 32) != 0 || memcmp(g.enc.pub, h.enc_pub, 32) != 0 || strcmp(roll_hash, h.next) != 0) {
        if (announced_earlier(chain, clen, g.sign.pub, h.seq))
            seterr(err, errlen, "that is an older recovery phrase for this identity: its keys were retired when you "
                   "changed your keys. Use the phrase you wrote down most recently");
        else
            seterr(err, errlen, "that phrase doesn't belong to this chain. It may be a different identity's, or the "
                   "chain may be out of date: get a newer copy (from a backup, or a friend) and try again");
        goto out;
    }

    id = blank(data_dir);
    if (!id) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    id->sign = g.sign;
    id->enc = g.enc;
    memcpy(id->id, h.id, sizeof id->id);
    id->seq = h.seq;
    memcpy(id->head_hash, h.head_hash, 65);
    memcpy(id->next, h.next, 65);
    if (install_identity(data_dir, id->dir, id->id, &g.sign, &g.enc, chain, clen, 0, err, errlen) != 0) {
        puddle_identity_close(id);
        id = NULL;
    }
out:
    memset(&g, 0, sizeof g);
    memset(canon, 0, sizeof canon);
    free(chain);
    return id;
}

/* -------------------------------------------------------------------- open */

puddle_identity *puddle_identity_open(const char *data_dir, char *err, size_t errlen)
{
    if (!puddle_identity_exists(data_dir)) {
        seterr(err, errlen, "this Puddle has no identity yet; run `puddled --init` in a terminal to make one "
               "and see your recovery phrase");
        return NULL;
    }
    if (puddle_identity_unconfirmed(data_dir)) {
        seterr(err, errlen, "this Puddle's identity was never confirmed: its recovery phrase was shown but "
               "not marked as written down, so it can't be used. Start setup again (the app, or "
               "`puddled --init`) to make a fresh one");
        return NULL;
    }
    puddle_identity *id = blank(data_dir);
    if (!id) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    if (apply_pending(id, 1, err, errlen) != 0) /* a roll that was interrupted is finished before anything else */
        goto fail;
    char file_id[PUDDLE_USER_ID_LEN + 1];
    int kr; kr = read_keys(id, file_id, err, errlen);
    if (kr < 0)
        goto fail;
    if (kr == 1) {
        seterr(err, errlen, "%s is missing; restore it from a backup, or restore your identity from your "
               "recovery phrase", id->keys_path);
        goto fail;
    }

    size_t clen; clen = 0;
    char *chain; chain = read_all(id->chain_path, &clen);
    if (!chain || clen == 0) {
        free(chain);
        seterr(err, errlen, "%s is missing or empty; restore it from a backup", id->chain_path);
        goto fail;
    }
    puddle_chain_head h;
    char why[200];
    int rc; rc = puddle_chain_verify(chain, clen, NULL, &h, why, sizeof why);
    free(chain);
    if (rc != 0) {
        seterr(err, errlen, "%s: %s", id->chain_path, why);
        goto fail;
    }
    if (strcmp(file_id, h.id) != 0) {
        seterr(err, errlen, "%s and %s belong to different identities (or one was made before the id format changed "
               "in this development version; if so, move %s aside and make a new identity with `puddled --init`)",
               id->keys_path, id->chain_path, id->dir);
        goto fail;
    }
    if (memcmp(h.sign_pub, id->sign.pub, 32) != 0 || memcmp(h.enc_pub, id->enc.pub, 32) != 0) {
        seterr(err, errlen, "the keys in %s are not the ones %s vouches for; restore both from the same backup",
               id->keys_path, id->chain_path);
        goto fail;
    }
    memcpy(id->id, h.id, sizeof id->id);
    id->seq = h.seq;
    memcpy(id->head_hash, h.head_hash, 65);
    memcpy(id->next, h.next, 65);
    return id;
fail:
    puddle_identity_close(id);
    return NULL;
}

void puddle_identity_close(puddle_identity *id)
{
    if (!id)
        return;
    memset(&id->sign, 0, sizeof id->sign);
    memset(&id->enc, 0, sizeof id->enc);
    delete id;
}

void puddle_identity_id(puddle_identity *id, char out[PUDDLE_USER_ID_LEN + 1])
{
    memcpy(out, id->id, PUDDLE_USER_ID_LEN + 1);
}

void puddle_identity_public(puddle_identity *id, unsigned char sign_pub[32], unsigned char enc_pub[32],
                            uint64_t *seq)
{
    id->mu.lock();
    if (sign_pub) memcpy(sign_pub, id->sign.pub, 32);
    if (enc_pub) memcpy(enc_pub, id->enc.pub, 32);
    if (seq) *seq = id->seq;
    id->mu.unlock();
}

int puddle_identity_sign(puddle_identity *id, const void *data, size_t len, unsigned char sig[64])
{
    id->mu.lock();
    int rc = sign_with(&id->sign, data, len, sig);
    id->mu.unlock();
    return rc;
}

char *puddle_identity_chain(puddle_identity *id, size_t *len)
{
    id->mu.lock();
    char *text = read_all(id->chain_path, len);
    id->mu.unlock();
    return text;
}
