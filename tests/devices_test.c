/* Pairing phones and letting them use the owner API (docs/identity.md, "Devices"), through the real route table: the device
 * list on its own, a phone pairing with the one-time secret, the owner accepting it, every answer to it signed, what a
 * device may and may not reach, and revoking it. */
#include "core/devices.h"
#include "http/router.h"
#include "service/routes.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "core/phrase.h"
#include "http/reqsig.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL
#define FP "abababababababababababababababababababababababababababababababab"

static char root[512], data[700];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

static puddle_identity *me;
static char my_id[PUDDLE_USER_ID_LEN + 1];
static unsigned char my_pub[32];
static puddle_ctx ctx;
static puddle_router R;
static puddle_config cfg;

typedef struct {
    int status;
    char out[40000];
    size_t n;
    char nonce[48];   /* of the request, for checking the receipt */
} reply;

/* a request as the owner token (owner = 1), as device `k` (signed), or as nobody */
static reply call(const char *method, const char *target, const char *body, int owner, const puddle_devkey *k)
{
    reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    memset(&rp, 0, sizeof rp);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target);
    snprintf(req.path, sizeof req.path, "%s", target);
    size_t bl = body ? strlen(body) : 0;
    req.content_length = bl;
    if (k) {
        CHECK(puddle_reqsig_sign_as(k->id, k->pub, puddle_devkey_sign, (void *)k, method, target, body, bl, my_id, clock_now, req.signature,
                                    sizeof req.signature) == 0);
        puddle_reqsig s;
        if (puddle_reqsig_parse(req.signature, &s) == 0)
            snprintf(rp.nonce, sizeof rp.nonce, "%s", s.nonce);
    }
    unsigned char *copy = malloc(bl + 1);
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    rp.n = puddle_router_handle(&R, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    rp.out[rp.n] = 0;
    free(copy);
    clock_now += 10;
    return rp;
}

/* the same request again, exactly (a replay) */
static reply replay_of(const char *method, const char *target, const puddle_devkey *k, char *sig_out)
{
    reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    memset(&rp, 0, sizeof rp);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target);
    snprintf(req.path, sizeof req.path, "%s", target);
    if (!sig_out[0])
        CHECK(puddle_reqsig_sign_as(k->id, k->pub, puddle_devkey_sign, (void *)k, method, target, NULL, 0, my_id, clock_now, sig_out, 1024) == 0);
    snprintf(req.signature, sizeof req.signature, "%s", sig_out);
    unsigned char none = 0;
    rp.n = puddle_router_handle(&R, &req, 0, &none, 0, rp.out, sizeof rp.out - 1, &rp.status);
    rp.out[rp.n] = 0;
    return rp;
}

static const char *body_of(const reply *r)
{
    const char *b = strstr(r->out, "\r\n\r\n");
    return b ? b + 4 : "";
}

static int member(const reply *r, const char *key, char *out, size_t cap)
{
    const char *b = body_of(r);
    puddle_span v;
    return puddle_json_member(b, strlen(b), key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* is the answer signed for by this Puddle, for this request and this body, over our certificate? */
static int receipted(const reply *r)
{
    const char *h = strstr(r->out, "X-Puddle-Receipt: ");
    const char *end = strstr(r->out, "\r\n\r\n");
    if (!h || !end || h > end)
        return 0;
    h += strlen("X-Puddle-Receipt: ");
    char rc[PUDDLE_RECEIPT_MAX];
    size_t n = strcspn(h, "\r");
    if (n >= sizeof rc)
        return 0;
    memcpy(rc, h, n);
    rc[n] = 0;
    const char *b = end + 4;
    return puddle_receipt_verify(rc, my_id, my_pub, r->nonce, r->status, b, strlen(b), FP) == PUDDLE_RECEIPT_OK;
}

static int has_receipt(const reply *r) { return strstr(r->out, "X-Puddle-Receipt") != NULL; }

/* a fresh pairing secret, from the owner */
static void new_secret(char out[PUDDLE_PAIR_SECRET_LEN + 1])
{
    reply r = call("POST", "/api/v1/devices/pair", "{}", 1, NULL);
    char link[400] = "";
    CHECK(r.status == 201 && member(&r, "link", link, sizeof link) == 0);
    const char *s = strstr(link, "&secret=");
    out[0] = 0;
    if (s)
        snprintf(out, PUDDLE_PAIR_SECRET_LEN + 1, "%s", s + 8);
}

static char *pair_body(const puddle_devkey *k, const char *name, const char *secret)
{
    static char b[600];
    char hex[65];
    puddle_hex_encode(k->pub, 32, hex);
    snprintf(b, sizeof b, "{\"name\":\"%s\",\"key\":\"%s\",\"secret\":\"%s\"}", name, hex, secret);
    return b;
}

/* the owner's routes for one device take its id in the path */
static void device_body(char *out, size_t cap, const puddle_devkey *k) { snprintf(out, cap, "%s", k->id); }
static const char *dpath(const char *id, const char *verb)
{
    static char p[4][200];
    static int i;
    i = (i + 1) % 4;
    snprintf(p[i], sizeof p[i], "/api/v1/devices/%s/%s", id, verb);
    return p[i];
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-devices-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/me", root);
    me = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!me) return 2;
    puddle_identity_id(me, my_id);
    puddle_identity_public(me, my_pub, NULL, NULL);
    snprintf(data, sizeof data, "%s/data", root);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", data);
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.address, sizeof cfg.address, "0.0.0.0"); /* every interface: says nothing about where to find us */
    cfg.port = 8443;
    puddle_devices *devs = puddle_devices_open(data, err, sizeof err);
    CHECK(devs != NULL);
    ctx = (puddle_ctx){ .config = &cfg, .identity = me, .devices = devs, .replay = puddle_replay_new(4096), .now_ms = now_cb };
    snprintf(ctx.own_tls_fp, sizeof ctx.own_tls_fp, "%s", FP);
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply r;
    char body[400], secret[PUDDLE_PAIR_SECRET_LEN + 1], s[200];

    /* ---- a device's key ---- */
    puddle_devkey phone, other, third;
    CHECK(puddle_devkey_generate(&phone) == 0 && puddle_devkey_generate(&other) == 0 && puddle_devkey_generate(&third) == 0);
    CHECK(strlen(phone.id) == PUDDLE_DEVICE_ID_LEN && puddle_id_valid(phone.id) && strcmp(phone.id, other.id) != 0);
    {
        puddle_devkey again;
        CHECK(puddle_devkey_from_seed(&again, phone.priv) == 0 && !memcmp(again.pub, phone.pub, 32) && !strcmp(again.id, phone.id));
        unsigned char sig[64];
        CHECK(puddle_devkey_sign(&phone, "hello", 5, sig) == 0 && puddle_verify_signature(phone.pub, "hello", 5, sig));
        CHECK(!puddle_verify_signature(other.pub, "hello", 5, sig));
        char c1[7], c2[7];
        puddle_device_check(phone.pub, c1);
        puddle_device_check(phone.pub, c2);
        CHECK(strlen(c1) == 6 && strspn(c1, "0123456789") == 6 && !strcmp(c1, c2));
    }

    /* ---- the owner asks for a pairing link: only with an address to give ---- */
    CHECK(call("GET", "/api/v1/devices", NULL, 1, NULL).status == 200);
    r = call("GET", "/api/v1/devices", NULL, 1, NULL);
    CHECK(strstr(body_of(&r), "\"devices\":[]"));
    r = call("POST", "/api/v1/devices/pair", "{}", 1, NULL);
    CHECK(r.status == 409 && strstr(body_of(&r), "no_address"));
    snprintf(cfg.advertise, sizeof cfg.advertise, "203.0.113.7:8443");
    r = call("POST", "/api/v1/devices/pair", "{}", 1, NULL);
    {
        char link[400] = "", want[200];
        CHECK(r.status == 201 && member(&r, "link", link, sizeof link) == 0);
        snprintf(want, sizeof want, "puddle-pair://203.0.113.7:8443?id=%s&secret=", my_id);
        CHECK(!strncmp(link, want, strlen(want)) && strlen(link) == strlen(want) + PUDDLE_PAIR_SECRET_LEN);
        CHECK(strstr(body_of(&r), "\"expires_at\":"));
    }
    CHECK(call("POST", "/api/v1/devices/pair", "{}", 0, NULL).status == 404);               /* nobody else may ask */

    /* ---- the phone pairs ---- */
    new_secret(secret);
    /* without the secret, or with a signature that is not the key's, nothing: the plain 404 a stranger always gets */
    {
        char bad[PUDDLE_PAIR_SECRET_LEN + 1];
        snprintf(bad, sizeof bad, "%s", secret);
        bad[0] = bad[0] == '0' ? '1' : '0';
        r = call("POST", "/device/v1/pair", pair_body(&phone, "Alex's phone", bad), 0, &phone);
        CHECK(r.status == 404 && !has_receipt(&r));
        r = call("POST", "/device/v1/pair", pair_body(&phone, "Alex's phone", secret), 0, NULL);
        CHECK(r.status == 404 && !has_receipt(&r));                                       /* unsigned */
        r = call("POST", "/device/v1/pair", pair_body(&phone, "Alex's phone", secret), 0, &other);
        CHECK(r.status == 404 && !has_receipt(&r));                                       /* signed by another key */
    }
    r = call("POST", "/device/v1/pair", pair_body(&phone, "  ", secret), 0, &phone);
    CHECK(r.status == 400 && receipted(&r));                                              /* it had the secret: it is told, in our name */
    r = call("POST", "/device/v1/pair", pair_body(&phone, "Alex's phone", secret), 0, &phone);
    CHECK(r.status == 202 && receipted(&r));
    {
        char dev[80] = "", check[16] = "", want_check[7], pid[80] = "";
        puddle_device_check(phone.pub, want_check);
        CHECK(member(&r, "device", dev, sizeof dev) == 0 && !strcmp(dev, phone.id));
        CHECK(member(&r, "check", check, sizeof check) == 0 && !strcmp(check, want_check));
        CHECK(strstr(body_of(&r), "\"state\":\"pending\""));
        /* what the phone does next: the chain is this Puddle's (ids are self-certifying), and its key signed the receipt */
        const char *b = body_of(&r);
        puddle_span pv, v;
        static char chain[20000];
        CHECK(puddle_json_member(b, strlen(b), "puddle", &pv) == 0);
        CHECK(puddle_json_member(pv.p, pv.len, "id", &v) == 0 && puddle_json_string(v, pid, sizeof pid) == 0 && !strcmp(pid, my_id));
        CHECK(puddle_json_member(pv.p, pv.len, "chain", &v) == 0 && puddle_json_string(v, chain, sizeof chain) == 0);
        puddle_chain_head h;
        CHECK(puddle_chain_verify(chain, strlen(chain), my_id, &h, err, sizeof err) == 0 && !memcmp(h.sign_pub, my_pub, 32));
    }
    /* the secret is spent: the next phone needs a new one */
    r = call("POST", "/device/v1/pair", pair_body(&other, "Tablet", secret), 0, &other);
    CHECK(r.status == 404 && !has_receipt(&r));
    /* and a secret lasts ten minutes */
    new_secret(secret);
    clock_now += PUDDLE_PAIR_MS;
    r = call("POST", "/device/v1/pair", pair_body(&other, "Tablet", secret), 0, &other);
    CHECK(r.status == 404);
    /* a new one replaces the old */
    {
        char old[PUDDLE_PAIR_SECRET_LEN + 1];
        new_secret(old);
        new_secret(secret);
        CHECK(strcmp(old, secret) != 0);
        CHECK(call("POST", "/device/v1/pair", pair_body(&other, "Tablet", old), 0, &other).status == 404);
        /* names: trimmed, at most 64 bytes, nothing unprintable */
        char longname[80];
        memset(longname, 'x', 65);
        longname[65] = 0;
        CHECK(call("POST", "/device/v1/pair", pair_body(&other, longname, secret), 0, &other).status == 400);
        CHECK(call("POST", "/device/v1/pair", pair_body(&other, "tab\\u0007let", secret), 0, &other).status == 400);
        CHECK(call("POST", "/device/v1/pair", "{\"key\":\"zz\",\"secret\":\"x\"}", 0, &other).status == 404);
        r = call("POST", "/device/v1/pair", pair_body(&other, "  Tablet ", secret), 0, &other);
        CHECK(r.status == 202);
    }

    /* ---- waiting: the phone may ask how it stands, and nothing else ---- */
    r = call("GET", "/device/v1/status", NULL, 0, &phone);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"state\":\"pending\"") && receipted(&r));
    r = call("GET", "/device/v1/status", NULL, 0, &third);                                 /* never paired */
    CHECK(r.status == 404 && !has_receipt(&r));
    r = call("GET", "/health", NULL, 0, &phone);
    CHECK(r.status == 404 && !has_receipt(&r));                                           /* not yet */

    /* ---- the owner sees them, with the check code, and accepts the phone ---- */
    r = call("GET", "/api/v1/devices", NULL, 1, NULL);
    {
        char c[7];
        puddle_device_check(phone.pub, c);
        CHECK(r.status == 200 && strstr(body_of(&r), phone.id) && strstr(body_of(&r), other.id) && strstr(body_of(&r), c));
        CHECK(strstr(body_of(&r), "\"name\":\"Alex's phone\"") && strstr(body_of(&r), "\"name\":\"Tablet\""));
        const char *t = strstr(body_of(&r), other.id), *p = strstr(body_of(&r), phone.id);
        CHECK(t && p && t < p);                                                             /* newest first */
    }
    device_body(body, sizeof body, &phone);
    CHECK(call("POST", dpath(body, "accept"), "{}", 0, NULL).status == 404);              /* the owner's to do */
    r = call("POST", dpath(body, "accept"), "{}", 1, NULL);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"state\":\"active\""));
    CHECK(call("POST", dpath(body, "accept"), "{}", 1, NULL).status == 409);
    CHECK(call("POST", dpath(body, "decline"), "{}", 1, NULL).status == 409);             /* accepted: revoke it instead */
    CHECK(call("POST", dpath("nope", "accept"), "{}", 1, NULL).status == 404);                 /* not a device id: no such path */
    device_body(s, sizeof s, &third);
    CHECK(call("POST", dpath(s, "accept"), "{}", 1, NULL).status == 404);
    r = call("GET", "/device/v1/status", NULL, 0, &phone);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"state\":\"active\"") && receipted(&r));

    /* ---- accepted: the phone is the owner, and every answer is signed ---- */
    r = call("GET", "/health", NULL, 0, &phone);
    CHECK(r.status == 200 && receipted(&r));
    r = call("GET", "/api/v1/identity", NULL, 0, &phone);
    CHECK(r.status == 200 && strstr(body_of(&r), my_id) && receipted(&r));
    r = call("GET", "/health", NULL, 1, NULL);
    CHECK(r.status == 200 && !has_receipt(&r));                                           /* the owner token is local: nothing to prove */
    {
        /* a body is read for an accepted device, before its signature can be checked; not for anyone else */
        puddle_request q;
        memset(&q, 0, sizeof q);
        snprintf(q.method, sizeof q.method, "POST");
        snprintf(q.path, sizeof q.path, "/api/v1/identity/roll/cancel");
        snprintf(q.target, sizeof q.target, "%s", q.path);
        CHECK(puddle_router_body_limit(&R, &q, 0) == 0);
        CHECK(puddle_router_body_limit(&R, &q, 1) > 0);
        CHECK(puddle_reqsig_sign_as(phone.id, phone.pub, puddle_devkey_sign, &phone, "POST", q.target, "{}", 2, my_id, clock_now, q.signature,
                                    sizeof q.signature) == 0);
        CHECK(puddle_router_body_limit(&R, &q, 0) > 0);
        CHECK(puddle_reqsig_sign_as(other.id, other.pub, puddle_devkey_sign, &other, "POST", q.target, "{}", 2, my_id, clock_now, q.signature,
                                    sizeof q.signature) == 0);
        CHECK(puddle_router_body_limit(&R, &q, 0) == 0);                                   /* still waiting */
        snprintf(q.path, sizeof q.path, "/api/v1/devices/%s/accept", other.id);
        snprintf(q.target, sizeof q.target, "%s", q.path);
        CHECK(puddle_reqsig_sign_as(phone.id, phone.pub, puddle_devkey_sign, &phone, "POST", q.target, "{}", 2, my_id, clock_now, q.signature,
                                    sizeof q.signature) == 0);
        CHECK(puddle_router_body_limit(&R, &q, 0) == 0);                                   /* devices are not a phone's to manage */
    }
    /* ...but not managing devices: a stolen phone lets nothing else in */
    device_body(s, sizeof s, &other);
    r = call("POST", dpath(s, "accept"), "{}", 0, &phone);
    CHECK(r.status == 404 && !has_receipt(&r));
    CHECK(call("GET", "/api/v1/devices", NULL, 0, &phone).status == 404);
    CHECK(call("POST", "/api/v1/devices/pair", "{}", 0, &phone).status == 404);
    /* a captured request can't be played again */
    {
        char sig[1024] = "";
        r = replay_of("GET", "/health", &phone, sig);
        CHECK(r.status == 200);
        r = replay_of("GET", "/health", &phone, sig);
        CHECK(r.status == 401 && strstr(body_of(&r), "replayed") && has_receipt(&r));
    }
    /* last seen, for the list */
    r = call("GET", "/api/v1/devices", NULL, 1, NULL);
    CHECK(strstr(body_of(&r), "\"last_seen\":"));

    /* ---- kept across a restart (the secret is not) ---- */
    new_secret(secret);
    puddle_devices_close(devs);
    devs = puddle_devices_open(data, err, sizeof err);
    CHECK(devs != NULL);
    ctx.devices = devs;
    CHECK(call("GET", "/health", NULL, 0, &phone).status == 200);
    r = call("GET", "/device/v1/status", NULL, 0, &other);
    CHECK(r.status == 200 && strstr(body_of(&r), "pending"));
    CHECK(call("POST", "/device/v1/pair", pair_body(&third, "Watch", secret), 0, &third).status == 404);

    /* ---- declining forgets a waiting one; revoking cuts an accepted one off ---- */
    device_body(s, sizeof s, &other);
    CHECK(call("POST", dpath(s, "revoke"), "{}", 0, &phone).status == 404);
    CHECK(call("POST", dpath(s, "decline"), "{}", 1, NULL).status == 204);
    CHECK(call("GET", "/device/v1/status", NULL, 0, &other).status == 404);
    CHECK(call("POST", dpath(s, "decline"), "{}", 1, NULL).status == 404);
    device_body(body, sizeof body, &phone);
    CHECK(call("POST", dpath(body, "revoke"), "{}", 1, NULL).status == 204);
    r = call("GET", "/health", NULL, 0, &phone);
    CHECK(r.status == 404 && !has_receipt(&r));                                           /* gone, like a stranger */
    CHECK(call("GET", "/device/v1/status", NULL, 0, &phone).status == 404);
    CHECK(call("POST", dpath(body, "revoke"), "{}", 1, NULL).status == 404);
    r = call("GET", "/api/v1/devices", NULL, 1, NULL);
    CHECK(strstr(body_of(&r), "\"devices\":[]"));
    /* and it was saved */
    puddle_devices_close(devs);
    devs = puddle_devices_open(data, err, sizeof err);
    ctx.devices = devs;
    CHECK(call("GET", "/health", NULL, 0, &phone).status == 404);

    /* ---- the list has a limit ---- */
    {
        int ok = 1;
        for (int i = 0; i < PUDDLE_DEVICES_MAX; i++) {
            puddle_devkey k;
            puddle_devkey_generate(&k);
            new_secret(secret);
            ok &= call("POST", "/device/v1/pair", pair_body(&k, "one of many", secret), 0, &k).status == 202;
        }
        CHECK(ok);
        new_secret(secret);
        r = call("POST", "/device/v1/pair", pair_body(&third, "one too many", secret), 0, &third);
        CHECK(r.status == 409 && strstr(body_of(&r), "too_many_devices"));
    }

    /* ---- a damaged list is not quietly emptied ---- */
    puddle_devices_close(devs);
    {
        char p[900];
        snprintf(p, sizeof p, "%s/state/devices.json", data);
        FILE *f = fopen(p, "wb");
        fputs("{\"v\":1,\"devices\":[{\"name\":\"x\"}]}", f);
        fclose(f);
        CHECK(puddle_devices_open(data, err, sizeof err) == NULL && strstr(err, "damaged"));
    }

    puddle_replay_free(ctx.replay);
    puddle_identity_close(me);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
