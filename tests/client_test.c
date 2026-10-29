#include "http/client.h"

#include "core/phrase.h"
#include "http/reqsig.h"
#include "tlsserver.h"

#include <ftw.h>
#include <sys/stat.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static puddle_address addr_of(unsigned port)
{
    char t[64];
    puddle_address a;
    snprintf(t, sizeof t, "127.0.0.1:%u", port);
    if (puddle_address_parse(t, &a) != 0) exit(2);
    return a;
}

struct look { const char *id, *kid; unsigned char pub[32]; };
static int found(void *u, const char *s, const char *k, unsigned char out[32])
{
    struct look *l = u;
    if (strcmp(s, l->id) || strcmp(k, l->kid)) return 0;
    memcpy(out, l->pub, 32);
    return 1;
}

static int seen_fp(void *u, const char *fp) { snprintf((char *)u, 65, "%s", fp); return 1; }
static int refuse_fp(void *u, const char *fp) { (void)u; (void)fp; return 0; }

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-client-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/alice", root);
    puddle_identity *alice = puddle_identity_create(dir, phrase, err, sizeof err);
    snprintf(dir, sizeof dir, "%s/bob", root);
    puddle_identity *bob = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!alice || !bob) return 2;
    puddle_identity_confirm(dir);
    char alice_id[PUDDLE_USER_ID_LEN + 1], bob_id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(alice, alice_id);
    puddle_identity_id(bob, bob_id);

    testsrv srv;
    snprintf(dir, sizeof dir, "%s/srv", root);
    if (ts_start(&srv, dir, 0) != 0) { fprintf(stderr, "cannot start the test server\n"); return 2; }
    puddle_address at = addr_of(srv.port);
    puddle_net_reply r;
    puddle_net_opts opts = { .connect_timeout_s = 2, .io_timeout_s = 2, .total_timeout_s = 5 };
    const char *body = "{\"hello\":\"world\"}";

    /* ---- a request that works ---- */
    ts_set(&srv, TS_NORMAL, 200, "{\"ok\":true}", 0);
    char fp[65] = "";
    opts.fingerprint_ok = seen_fp;
    opts.user = fp;
    puddle_net_request(&at, "POST", "/stream/v1/access/result", body, strlen(body), alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OK && r.status == 200 && r.body && !strcmp(r.body, "{\"ok\":true}") && r.body_len == 11);
    CHECK(!strcmp(r.fingerprint, srv.fingerprint) && !strcmp(fp, srv.fingerprint));           /* the certificate's SHA-256 */
    CHECK(!strcmp(r.content_type, "application/json"));                                          /* what it said it was sending */
    CHECK(srv.requests == 1 && !strcmp(srv.method, "POST") && !strcmp(srv.target, "/stream/v1/access/result"));
    CHECK(srv.body_len == strlen(body) && !memcmp(srv.body, body, srv.body_len));
    {
        /* what arrived names Alice and her key */
        unsigned char pub[32];
        puddle_identity_public(alice, pub, NULL, NULL);
        puddle_reqsig sig;
        CHECK(puddle_reqsig_parse(srv.signature, &sig) == 0 && !strcmp(sig.id, alice_id));
        char kid[65];
        puddle_key_id(pub, kid);
        CHECK(!strcmp(sig.kid, kid));
    }
    puddle_net_reply_free(&r);
    CHECK(r.body == NULL);

    /* the signature really does check out against Alice's key, for Bob and no one else */
    {
        unsigned char pub[32];
        puddle_identity_public(alice, pub, NULL, NULL);
        char kid[65];
        puddle_key_id(pub, kid);
        struct look lk = { alice_id, kid, { 0 } };
        memcpy(lk.pub, pub, 32);
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        int64_t now = (int64_t)t.tv_sec * 1000;
        puddle_replay *rp = puddle_replay_new(64);
        CHECK(puddle_reqsig_authenticate(srv.signature, "POST", "/stream/v1/access/result", body, strlen(body), bob_id, found, &lk, rp, now, NULL) == PUDDLE_AUTHN_OK);
        puddle_replay_free(rp);
        rp = puddle_replay_new(64);
        CHECK(puddle_reqsig_authenticate(srv.signature, "POST", "/stream/v1/access/result", body, strlen(body), alice_id, found, &lk, rp, now, NULL) == PUDDLE_AUTHN_REJECT);
        CHECK(puddle_reqsig_authenticate(srv.signature, "POST", "/stream/v1/access/result", "{}", 2, bob_id, found, &lk, rp, now, NULL) == PUDDLE_AUTHN_REJECT);
        puddle_replay_free(rp);
    }

    /* a GET, and answers of every shape */
    puddle_net_request(&at, "GET", "/stream/v1/access/status", NULL, 0, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OK && r.status == 200 && !strcmp(srv.method, "GET") && srv.body_len == 0);
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_NORMAL, 404, "{\"error\":\"Not Found\"}", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OK && r.status == 404 && r.body && strstr(r.body, "Not Found"));   /* it is for the caller to judge a 404 */
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_NORMAL, 202, "", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OK && r.status == 202 && r.body_len == 0);
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_NORMAL, 204, "", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OK && r.status == 204);
    puddle_net_reply_free(&r);

    /* ---- the certificate is not what it was ---- */
    {
        int before = srv.requests;
        opts.fingerprint_ok = refuse_fp;
        puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
        CHECK(r.result == PUDDLE_NET_CERT_CHANGED && r.reason[0] && !strcmp(r.fingerprint, srv.fingerprint));
        CHECK(srv.requests == before);                                                            /* nothing was sent to it */
        puddle_net_reply_free(&r);
        opts.fingerprint_ok = NULL;
    }

    /* ---- the other end misbehaves: these fail, and are not "offline" ---- */
    ts_set(&srv, TS_CLOSE, 200, "", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_FAILED && r.reason[0]);
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_PLAIN, 200, "", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_FAILED && strstr(r.reason, "TLS") && r.fingerprint[0] == '\0');      /* not TLS */
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_HANG, 200, "", 0);
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
        clock_gettime(CLOCK_MONOTONIC, &b);
        CHECK(r.result == PUDDLE_NET_FAILED);                                                     /* accepted, never spoke TLS */
        CHECK(b.tv_sec - a.tv_sec <= 4);                                                          /* and gave up when told to */
        puddle_net_reply_free(&r);
    }
    ts_set(&srv, TS_SLOW, 200, "", 4000);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_FAILED && r.fingerprint[0]);                                     /* TLS fine, then silence */
    puddle_net_reply_free(&r);
    ts_set(&srv, TS_BIG, 200, "", 0);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    if (!(r.result == PUDDLE_NET_FAILED && strstr(r.reason, "large"))) fprintf(stderr, "big: %d %d %s\n", r.result, r.status, r.reason);
    CHECK(r.result == PUDDLE_NET_FAILED && strstr(r.reason, "large"));                            /* more than we will read */
    puddle_net_reply_free(&r);
    /* a bigger answer, when the caller says how big it will take */
    {
        puddle_net_opts big = opts;
        big.max_body = 100000;
        ts_set(&srv, TS_BIG, 200, "", 0);
        puddle_net_request(&at, "GET", "/x", NULL, 0, alice, bob_id, &big, &r);
        CHECK(r.result == PUDDLE_NET_OK && r.status == 200 && r.body_len == PUDDLE_NET_BODY_MAX + 20000 && r.body[0] == 'x' && r.body[r.body_len - 1] == 'x');
        puddle_net_reply_free(&r);
        big.max_body = PUDDLE_NET_BODY_MAX + 19999;                                               /* one byte less than it has */
        puddle_net_request(&at, "GET", "/x", NULL, 0, alice, bob_id, &big, &r);
        CHECK(r.result == PUDDLE_NET_FAILED && strstr(r.reason, "large"));
        puddle_net_reply_free(&r);
    }
    ts_set(&srv, TS_NORMAL, 200, "{}", 0);

    /* ---- nobody there: offline ---- */
    ts_stop(&srv);
    puddle_net_request(&at, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
    CHECK(r.result == PUDDLE_NET_OFFLINE && strstr(r.reason, "cannot reach"));                    /* connection refused */
    puddle_net_reply_free(&r);
    {
        puddle_address a;
        CHECK(puddle_address_parse("no-such-host.invalid:8443", &a) == 0);
        puddle_net_request(&a, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
        CHECK(r.result == PUDDLE_NET_OFFLINE && strstr(r.reason, "no-such-host.invalid"));        /* cannot find it: try again later */
        puddle_net_reply_free(&r);
    }

    /* ---- addresses we will not dial ---- */
    {
        const char *no[] = { "0.0.0.0:8443", "0.1.2.3:8443", "169.254.169.254:80", "224.0.0.1:8443", "255.255.255.255:8443", "240.0.0.1:8443",
                             "[::]:8443", "[fe80::1]:8443", "[ff02::1]:8443", "[::ffff:a9fe:a9fe]:80", "[::ffff:0:0]:80" };
        for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
            puddle_address a;
            CHECK(puddle_address_parse(no[i], &a) == 0);
            puddle_net_request(&a, "POST", "/x", "{}", 2, alice, bob_id, &opts, &r);
            if (r.result != PUDDLE_NET_FAILED || !strstr(r.reason, "not an address we will connect to")) {
                fprintf(stderr, "FAIL dialled or mis-reported %s: %d %s\n", no[i], r.result, r.reason);
                failures++;
            }
            puddle_net_reply_free(&r);
        }
        CHECK(!puddle_net_host_dialable("169.254.1.1") && !puddle_net_host_dialable("0.0.0.0") && !puddle_net_host_dialable("::"));
        CHECK(puddle_net_host_dialable("127.0.0.1") && puddle_net_host_dialable("10.1.2.3") && puddle_net_host_dialable("192.168.1.1") && puddle_net_host_dialable("203.0.113.7"));
        CHECK(puddle_net_host_dialable("::1") && puddle_net_host_dialable("2001:db8::1") && puddle_net_host_dialable("fd00::1"));
    }

    /* a request that cannot be signed (a recipient that is not an id) goes nowhere */
    {
        puddle_address a = addr_of(srv.port);
        puddle_net_request(&a, "POST", "/x", "{}", 2, alice, "nope", &opts, &r);
        CHECK(r.result == PUDDLE_NET_FAILED && strstr(r.reason, "sign"));
        puddle_net_reply_free(&r);
    }

    puddle_identity_close(alice);
    puddle_identity_close(bob);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
