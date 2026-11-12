/* The Puddle Protocol's test vectors (ezp2p/protocol, vectors/): fixed inputs and the answer every implementation must give.
 *
 * By default this checks Puddle against them: it looks for the protocol checkout beside this one (../protocol, or
 * $PUDDLE_PROTOCOL_DIR) and skips if there is none, as Pond's end-to-end test does without a Puddle. With
 * PUDDLE_WRITE_VECTORS=<dir> it writes them instead, from Puddle's own code, which is the reference
 * (protocol/decisions.md). Writing them again makes new signatures (new nonces, a new identity), so do that only when the
 * protocol changes. */
#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "core/devices.h"
#include "core/identity.h"
#include "core/phrase.h"
#include "http/reqsig.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checked;
#define CHECK(cond) do { checked++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define NOW 1760000000000LL /* the time every request vector is checked at */
#define FP_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define FP_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static const unsigned char SEEDS[3][32] = {
    { 0 },
    { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
    { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8, 0xf7, 0xf6, 0xf5, 0xf4, 0xf3, 0xf2, 0xf1, 0xf0,
      0xef, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xe0 },
};

static void hex(const unsigned char *b, unsigned n, char *out) { puddle_hex_encode(b, n, out); }

/* a JSON string, escaped (vectors hold only printable ASCII, and bodies with quotes) */
static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/* ---------------------------------------------------------------- writing */

static int write_devices(const char *dir)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/devices.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "{\n  \"about\": \"Device ids and check codes (spec/01-identity.md): an Ed25519 key from each 32-byte seed, its public key, "
               "base32(SHA-256(\\\"puddle-device-v1\\\\n\\\" || public key)), and the six-digit check code from the same hash.\",\n");
    fprintf(f, "  \"devices\": [\n");
    for (int i = 0; i < 3; i++) {
        puddle_devkey k;
        char seed[65], pub[65], check[7];
        if (puddle_devkey_from_seed(&k, SEEDS[i]) != 0)
            return -1;
        hex(SEEDS[i], 32, seed);
        hex(k.pub, 32, pub);
        puddle_device_check(k.pub, check);
        fprintf(f, "    { \"seed\": \"%s\", \"public_key\": \"%s\", \"id\": \"%s\", \"check\": \"%s\" }%s\n", seed, pub, k.id, check,
                i < 2 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

typedef struct {
    const char *name, *expect;    /* what a checker must say: ok, reject, clock_skew */
    const char *method, *target, *body, *recipient, *header_body; /* header_body: the body the header was made for, if not `body` */
    int64_t ts;
} req_case;

typedef struct {
    const char *name, *expect;    /* ok, missing, wrong_key, bad, other_cert */
    int status, signed_status;
    const char *body, *signed_body, *seen_fp, *signed_fp;
    int other_key, no_header;
} receipt_case;

static const char *check_name(puddle_receipt_check c)
{
    switch (c) {
    case PUDDLE_RECEIPT_OK: return "ok";
    case PUDDLE_RECEIPT_MISSING: return "missing";
    case PUDDLE_RECEIPT_WRONG_KEY: return "wrong_key";
    case PUDDLE_RECEIPT_BAD: return "bad";
    default: return "other_cert";
    }
}

static int write_signatures(const char *dir)
{
    char root[512], idir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-vectors-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return -1;
    puddle_identity *ids[2];
    for (int i = 0; i < 2; i++) {
        snprintf(idir, sizeof idir, "%s/%c", root, 'a' + i);
        ids[i] = puddle_identity_create(idir, phrase, err, sizeof err);
        if (!ids[i])
            return -1;
    }
    char sender[PUDDLE_USER_ID_LEN + 1], responder[PUDDLE_USER_ID_LEN + 1], other[PUDDLE_USER_ID_LEN + 1];
    unsigned char spub[32], rpub[32], opub[32];
    char spub_h[65], rpub_h[65], opub_h[65];
    puddle_identity_id(ids[0], sender);
    puddle_identity_id(ids[1], responder);
    puddle_identity_public(ids[0], spub, NULL, NULL);
    puddle_identity_public(ids[1], rpub, NULL, NULL);
    hex(spub, 32, spub_h);
    hex(rpub, 32, rpub_h);
    /* "someone else": a third key, for a receipt signed by the wrong party */
    puddle_devkey third;
    puddle_devkey_from_seed(&third, SEEDS[2]);
    memcpy(opub, third.pub, 32);
    hex(opub, 32, opub_h);
    snprintf(other, sizeof other, "%s", third.id);

    char path[1100];
    snprintf(path, sizeof path, "%s/signatures.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "{\n  \"about\": \"Request signatures and answer receipts (spec/02-signatures.md). Keys are hex here; in the headers they are as the "
               "spec says. A request is checked by its recipient at now_ms, knowing the sender's public key; a receipt by the caller, knowing the "
               "responder's public key, the request's nonce and the certificate it saw.\",\n");
    fprintf(f, "  \"now_ms\": %lld,\n", (long long)NOW);
    fprintf(f, "  \"sender\": { \"id\": \"%s\", \"public_key\": \"%s\" },\n", sender, spub_h);
    fprintf(f, "  \"responder\": { \"id\": \"%s\", \"public_key\": \"%s\" },\n", responder, rpub_h);

    const req_case reqs[] = {
        { "a POST, signed for its recipient", "ok", "POST", "/stream/v1/events", "{\"chat\":\"x\",\"events\":[]}", NULL, NULL, NOW - 1000 },
        { "a GET with a query", "ok", "GET", "/stream/v1/media?hash=0011", "", NULL, NULL, NOW },
        { "signed for anyone (a first request to an address)", "ok", "POST", "/stream/v1/access", "{}", "*", NULL, NOW },
        { "the body was changed", "reject", "POST", "/stream/v1/events", "{\"chat\":\"y\",\"events\":[]}", NULL, "{\"chat\":\"x\",\"events\":[]}", NOW },
        { "meant for someone else", "reject", "POST", "/stream/v1/events", "{}", "other", NULL, NOW },
        { "ten minutes old", "clock_skew", "POST", "/stream/v1/events", "{}", NULL, NULL, NOW - 10 * 60 * 1000 },
    };
    fprintf(f, "  \"requests\": [\n");
    for (size_t i = 0; i < sizeof reqs / sizeof *reqs; i++) {
        const req_case *c = &reqs[i];
        const char *recipient = c->recipient && !strcmp(c->recipient, "other") ? other : c->recipient ? c->recipient : responder;
        const char *hb = c->header_body ? c->header_body : c->body;
        char sig[1100];
        if (puddle_reqsig_sign(ids[0], c->method, c->target, hb, strlen(hb), recipient, c->ts, sig, sizeof sig) != 0)
            return -1;
        fprintf(f, "    { \"name\": ");
        jstr(f, c->name);
        fprintf(f, ", \"method\": \"%s\", \"target\": ", c->method);
        jstr(f, c->target);
        fprintf(f, ", \"body\": ");
        jstr(f, c->body);
        fprintf(f, ", \"checked_by\": \"%s\", \"accepts_any\": %s, \"header\": ", responder, c->recipient && !strcmp(c->recipient, "*") ? "true" : "false");
        jstr(f, sig);
        fprintf(f, ", \"expect\": \"%s\" }%s\n", c->expect, i + 1 < sizeof reqs / sizeof *reqs ? "," : "");
    }
    fprintf(f, "  ],\n");

    const receipt_case recs[] = {
        { "a good receipt", "ok", 200, 200, "{\"ok\":true}", NULL, FP_A, FP_A, 0, 0 },
        { "a good receipt for an empty answer", "ok", 204, 204, "", NULL, FP_A, FP_A, 0, 0 },
        { "a good refusal is still them", "ok", 403, 403, "{\"error\":\"no_stream\"}", NULL, FP_A, FP_A, 0, 0 },
        { "no receipt at all", "missing", 404, 404, "{\"error\":\"Not Found\"}", NULL, FP_A, FP_A, 0, 1 },
        { "signed by someone else", "wrong_key", 200, 200, "{}", NULL, FP_A, FP_A, 1, 0 },
        { "signed for another status", "bad", 200, 201, "{}", NULL, FP_A, FP_A, 0, 0 },
        { "signed for another body", "bad", 200, 200, "{\"a\":1}", "{\"a\":2}", FP_A, FP_A, 0, 0 },
        { "good, but over a certificate the caller did not see (intercepted)", "other_cert", 200, 200, "{}", NULL, FP_B, FP_A, 0, 0 },
    };
    fprintf(f, "  \"receipts\": [\n");
    for (size_t i = 0; i < sizeof recs / sizeof *recs; i++) {
        const receipt_case *c = &recs[i];
        char nonce[40], header[PUDDLE_RECEIPT_MAX] = "";
        unsigned char nb[16];
        for (int k = 0; k < 16; k++)
            nb[k] = (unsigned char)(i * 16 + k);
        puddle_base64_encode(nb, 16, nonce, sizeof nonce);
        const char *sb = c->signed_body ? c->signed_body : c->body;
        if (!c->no_header) {
            /* "signed by someone else": the other identity signs, naming itself */
            puddle_identity *who = c->other_key ? ids[0] : ids[1];
            if (puddle_receipt_sign(who, nonce, c->signed_status, sb, strlen(sb), c->signed_fp, header, sizeof header) != 0)
                return -1;
        }
        /* what Puddle says, so a vector can't disagree with the code that made it */
        puddle_receipt_check got = puddle_receipt_verify(header[0] ? header : NULL, responder, rpub, nonce, c->status, c->body, strlen(c->body), c->seen_fp);
        if (strcmp(check_name(got), c->expect) != 0) {
            fprintf(stderr, "vector '%s': Puddle says %s, not %s\n", c->name, check_name(got), c->expect);
            return -1;
        }
        fprintf(f, "    { \"name\": ");
        jstr(f, c->name);
        fprintf(f, ", \"nonce\": \"%s\", \"status\": %d, \"body\": ", nonce, c->status);
        jstr(f, c->body);
        fprintf(f, ", \"seen_fp\": \"%s\", \"header\": ", c->seen_fp);
        jstr(f, header);
        fprintf(f, ", \"expect\": \"%s\" }%s\n", c->expect, i + 1 < sizeof recs / sizeof *recs ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    puddle_identity_close(ids[0]);
    puddle_identity_close(ids[1]);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}

/* ---------------------------------------------------------------- checking */

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)malloc(1 << 20);
    *len = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (buf)
        buf[*len] = 0;
    return buf;
}

static int str_of(puddle_span obj, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(obj.p, obj.len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

static int hex32(const char *h, unsigned char out[32]) { return strlen(h) == 64 && puddle_hex_decode(h, out, 32) == 0 ? 0 : -1; }

static void check_devices(const char *dir)
{
    char path[1100];
    size_t n;
    snprintf(path, sizeof path, "%s/devices.json", dir);
    char *t = slurp(path, &n);
    CHECK(t != NULL);
    if (!t)
        return;
    puddle_span arr, item;
    CHECK(puddle_json_member(t, n, "devices", &arr) == 0);
    size_t cur = 0;
    int count = 0;
    while (puddle_json_array_next(arr, &cur, &item) == 1) {
        char seed_h[80], pub_h[80], id[80], check[16], c2[7];
        unsigned char seed[32], pub[32];
        CHECK(str_of(item, "seed", seed_h, sizeof seed_h) == 0 && hex32(seed_h, seed) == 0);
        CHECK(str_of(item, "public_key", pub_h, sizeof pub_h) == 0 && hex32(pub_h, pub) == 0);
        CHECK(str_of(item, "id", id, sizeof id) == 0 && str_of(item, "check", check, sizeof check) == 0);
        puddle_devkey k;
        CHECK(puddle_devkey_from_seed(&k, seed) == 0 && memcmp(k.pub, pub, 32) == 0 && strcmp(k.id, id) == 0);
        puddle_device_check(pub, c2);
        CHECK(strcmp(c2, check) == 0);
        count++;
    }
    CHECK(count == 3);
    free(t);
}

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    unsigned char pub[32];
} known;

static int known_key(void *user, const char *sender, const char *kid, unsigned char pub[32])
{
    const known *k = (const known *)user;
    char want[65];
    puddle_key_id(k->pub, want);
    if (strcmp(sender, k->id) != 0 || strcmp(kid, want) != 0)
        return 0;
    memcpy(pub, k->pub, 32);
    return 1;
}

static const char *authn_name(puddle_authn a)
{
    return a == PUDDLE_AUTHN_OK ? "ok" : a == PUDDLE_AUTHN_SKEW ? "clock_skew" : a == PUDDLE_AUTHN_REPLAYED ? "replayed" : "reject";
}

static void check_signatures(const char *dir)
{
    char path[1100];
    size_t n;
    snprintf(path, sizeof path, "%s/signatures.json", dir);
    char *t = slurp(path, &n);
    CHECK(t != NULL);
    if (!t)
        return;
    puddle_span top = { t, n }, s, arr, item, v;
    int64_t now = 0;
    CHECK(puddle_json_member(t, n, "now_ms", &v) == 0 && puddle_json_int64(v, &now) == 0);
    known sender, responder;
    char h[80];
    CHECK(puddle_json_member(t, n, "sender", &s) == 0 && str_of(s, "id", sender.id, sizeof sender.id) == 0 &&
          str_of(s, "public_key", h, sizeof h) == 0 && hex32(h, sender.pub) == 0);
    CHECK(puddle_json_member(t, n, "responder", &s) == 0 && str_of(s, "id", responder.id, sizeof responder.id) == 0 &&
          str_of(s, "public_key", h, sizeof h) == 0 && hex32(h, responder.pub) == 0);
    (void)top;

    size_t cur = 0;
    int count = 0;
    CHECK(puddle_json_member(t, n, "requests", &arr) == 0);
    while (puddle_json_array_next(arr, &cur, &item) == 1) {
        char name[200], method[8], target[300], body[600], by[80], header[1100], expect[32];
        CHECK(str_of(item, "name", name, sizeof name) == 0 && str_of(item, "method", method, sizeof method) == 0 &&
              str_of(item, "target", target, sizeof target) == 0 && str_of(item, "body", body, sizeof body) == 0 &&
              str_of(item, "checked_by", by, sizeof by) == 0 && str_of(item, "header", header, sizeof header) == 0 &&
              str_of(item, "expect", expect, sizeof expect) == 0);
        int any = puddle_json_member(item.p, item.len, "accepts_any", &v) == 0 && v.len == 4 && !strncmp(v.p, "true", 4);
        puddle_reqsig out;
        puddle_authn a = puddle_reqsig_authenticate(header, method, target, body, strlen(body), any ? PUDDLE_RECIPIENT_ANY : by, known_key,
                                                    &sender, NULL, now, &out);
        if (strcmp(authn_name(a), expect) != 0)
            fprintf(stderr, "request vector '%s': Puddle says %s, the vector %s\n", name, authn_name(a), expect);
        CHECK(strcmp(authn_name(a), expect) == 0);
        count++;
    }
    CHECK(count >= 6);

    cur = 0;
    count = 0;
    CHECK(puddle_json_member(t, n, "receipts", &arr) == 0);
    while (puddle_json_array_next(arr, &cur, &item) == 1) {
        char name[200], nonce[64], body[600], fp[80], header[PUDDLE_RECEIPT_MAX], expect[32];
        int64_t status = 0;
        CHECK(str_of(item, "name", name, sizeof name) == 0 && str_of(item, "nonce", nonce, sizeof nonce) == 0 &&
              str_of(item, "body", body, sizeof body) == 0 && str_of(item, "seen_fp", fp, sizeof fp) == 0 &&
              str_of(item, "header", header, sizeof header) == 0 && str_of(item, "expect", expect, sizeof expect) == 0 &&
              puddle_json_member(item.p, item.len, "status", &v) == 0 && puddle_json_int64(v, &status) == 0);
        puddle_receipt_check c = puddle_receipt_verify(header[0] ? header : NULL, responder.id, responder.pub, nonce, (int)status, body,
                                                       strlen(body), fp);
        if (strcmp(check_name(c), expect) != 0)
            fprintf(stderr, "receipt vector '%s': Puddle says %s, the vector %s\n", name, check_name(c), expect);
        CHECK(strcmp(check_name(c), expect) == 0);
        count++;
    }
    CHECK(count >= 8);
    free(t);
}

int main(void)
{
    const char *w = getenv("PUDDLE_WRITE_VECTORS");
    if (w && *w) {
        if (write_devices(w) != 0 || write_signatures(w) != 0) {
            fprintf(stderr, "could not write the vectors to %s\n", w);
            return 1;
        }
        printf("wrote the vectors to %s\n", w);
        return 0;
    }
    char dir[1100];
    const char *env = getenv("PUDDLE_PROTOCOL_DIR");
    snprintf(dir, sizeof dir, "%s/vectors", env && *env ? env : PUDDLE_SOURCE_DIR "/../protocol");
    struct stat st;
    if (stat(dir, &st) != 0) {
        printf("no protocol checkout at %s: skipped (set PUDDLE_PROTOCOL_DIR)\n", dir);
        return 0;
    }
    check_devices(dir);
    check_signatures(dir);
    printf("%d checks against %s\n", checked, dir);
    return failures ? 1 : 0;
}
