#include "http/router.h"

#include "api/api.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL

static char root[512];
static puddle_identity *alice, *bob, *carol;
static char bob_id[PUDDLE_USER_ID_LEN + 1], alice_id[PUDDLE_USER_ID_LEN + 1];
static unsigned char alice_pub[32];
static char alice_kid[65];

static puddle_identity *make(const char *name)
{
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    puddle_identity *id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (id) puddle_identity_confirm(dir);
    return id;
}

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

/* ---- handlers ---- */
static int h_owner(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "who", c->sender ? c->sender : "owner");
    puddle_jw_num(w, "body", (int64_t)c->body_len);
    puddle_jw_end_object(w);
    return 200;
}
static int h_empty(const puddle_call *c, puddle_jw *w) { (void)c; (void)w; return 202; }
static int h_params(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "user", puddle_param(c, "user"));
    puddle_jw_str(w, "message", puddle_param(c, "message"));
    puddle_jw_str(w, "none", puddle_param(c, "none"));
    puddle_jw_end_object(w);
    return 200;
}
static int h_toolong(const puddle_call *c, puddle_jw *w)
{
    (void)c;
    puddle_jw_object(w);
    for (int i = 0; i < 1000; i++) puddle_jw_str(w, "k", "a long enough value to overflow the buffer");
    puddle_jw_end_object(w);
    return 200;
}

static int chain_key(puddle_ctx *ctx, const unsigned char *body, size_t len, const char *sender,
                     const char *kid, unsigned char pub[32])
{
    (void)ctx;
    /* stands in for "the chain in the body proves this key": here the body must say so */
    if (len != 11 || memcmp(body, "alice-chain", 11) != 0 || strcmp(sender, alice_id) != 0 || strcmp(kid, alice_kid) != 0)
        return 0;
    memcpy(pub, alice_pub, 32);
    return 1;
}

static int peer_key(void *u, const char *sender, const char *kid, unsigned char pub[32])
{
    (void)u;
    if (strcmp(sender, alice_id) || strcmp(kid, alice_kid)) return 0;
    memcpy(pub, alice_pub, 32);
    return 1;
}

static const puddle_route routes[] = {
    { "GET",  "/o",      PUDDLE_AUTH_OWNER, 0,   h_owner, NULL },
    { "POST", "/o",      PUDDLE_AUTH_OWNER, 10,  h_owner, NULL },
    { "GET",  "/p",      PUDDLE_AUTH_PEER,  0,   h_owner, NULL },
    { "POST", "/p",      PUDDLE_AUTH_PEER,  100, h_owner, NULL },
    { "POST", "/c",      PUDDLE_AUTH_CHAIN, 100, h_owner, chain_key },
    { "GET",  "/empty",  PUDDLE_AUTH_OWNER, 0,   h_empty, NULL },
    { "GET",  "/toolong", PUDDLE_AUTH_OWNER, 0,  h_toolong, NULL },
    { "GET",  "/u/me",   PUDDLE_AUTH_OWNER, 0,   h_empty, NULL },
    { "GET",  "/u/{user:id}", PUDDLE_AUTH_OWNER, 0, h_params, NULL },
    { "POST", "/u/{user:id}/m/{message:event}/edit", PUDDLE_AUTH_OWNER, 10, h_params, NULL },
    { "POST", "/i/{token:token}/revoke", PUDDLE_AUTH_OWNER, 0, h_empty, NULL },
};

static puddle_ctx ctx;
static int64_t now_cb(void) { return T0; }

typedef struct { int status; char out[4096]; size_t n; } reply;

static reply call(const puddle_router *r, const char *method, const char *target, const char *sig, const char *body, int owner)
{
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target);
    snprintf(req.path, sizeof req.path, "%s", target);
    char *q = strchr(req.path, '?');
    if (q) *q = '\0';
    if (sig) snprintf(req.signature, sizeof req.signature, "%s", sig);
    size_t bl = body ? strlen(body) : 0;
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1); /* what the server hands over: exact size, NUL after it */
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    reply rp;
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(r, &req, owner, copy, bl, rp.out, sizeof rp.out, &rp.status);
    rp.out[rp.n < sizeof rp.out ? rp.n : sizeof rp.out - 1] = '\0';
    free(copy);
    return rp;
}

static const char *body_of(const reply *r)
{
    const char *b = strstr(r->out, "\r\n\r\n");
    return b ? b + 4 : "";
}

static void sign(puddle_identity *who, const char *method, const char *target, const char *body, const char *to, int64_t ts, char *out, size_t cap)
{
    if (puddle_reqsig_sign(who, method, target, body, body ? strlen(body) : 0, to, ts, out, cap) != 0) {
        fprintf(stderr, "sign failed\n");
        exit(2);
    }
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-router-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    alice = make("alice"); bob = make("bob"); carol = make("carol");
    if (!alice || !bob || !carol) { fprintf(stderr, "cannot make identities\n"); return 2; }
    puddle_identity_id(bob, bob_id);
    puddle_identity_id(alice, alice_id);
    puddle_identity_public(alice, alice_pub, NULL, NULL);
    puddle_key_id(alice_pub, alice_kid);

    ctx = (puddle_ctx){ .identity = bob, .replay = puddle_replay_new(64), .peer_key = peer_key, .now_ms = now_cb };
    puddle_router R = { routes, sizeof routes / sizeof routes[0], &ctx };
    reply r;
    char sig[600];

    /* ---- path parameters ---- */
    {
        char path[300], want[200];
        const char *ev = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        r = call(&R, "GET", "/u/me", NULL, NULL, 1);
        CHECK(r.status == 202);                                   /* a literal segment, not a malformed id */
        snprintf(path, sizeof path, "/u/%s", alice_id);
        r = call(&R, "GET", path, NULL, NULL, 1);
        CHECK(r.status == 200);
        snprintf(want, sizeof want, "\"user\":\"%s\"", alice_id);
        CHECK(strstr(body_of(&r), want) != NULL);
        CHECK(strstr(body_of(&r), "\"none\":\"\"") != NULL);
        r = call(&R, "GET", "/u/notanid", NULL, NULL, 1);
        CHECK(r.status == 404);                                   /* the wrong form is a path that does not exist */
        r = call(&R, "GET", path, NULL, NULL, 0);
        CHECK(r.status == 404);                                   /* and the owner check still applies */
        snprintf(path, sizeof path, "/u/%s/m/%s/edit", alice_id, ev);
        r = call(&R, "POST", path, NULL, "{}", 1);
        CHECK(r.status == 200 && strstr(body_of(&r), ev) != NULL);
        r = call(&R, "GET", path, NULL, NULL, 1);
        CHECK(r.status == 405);                                   /* the path is there, for POST */
        snprintf(path, sizeof path, "/u/%s/m/%.63s/edit", alice_id, ev);
        r = call(&R, "POST", path, NULL, "{}", 1);
        CHECK(r.status == 404);                                   /* 63 hex is not an event id */
        snprintf(path, sizeof path, "/u/%s/m/%s/edit/more", alice_id, ev);
        r = call(&R, "POST", path, NULL, "{}", 1);
        CHECK(r.status == 404);
        r = call(&R, "POST", "/i/abc-DEF_123/revoke", NULL, NULL, 1);
        CHECK(r.status == 202);
        r = call(&R, "POST", "/i/a.b/revoke", NULL, NULL, 1);
        CHECK(r.status == 404);
    }

    /* ---- owner routes ---- */
    r = call(&R, "GET", "/o", NULL, NULL, 1);
    CHECK(r.status == 200 && strstr(body_of(&r), "{\"who\":\"owner\",\"body\":0}"));
    r = call(&R, "POST", "/o", NULL, "hello", 1);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"body\":5"));
    reply stranger = call(&R, "GET", "/o", NULL, NULL, 0);
    reply nowhere = call(&R, "GET", "/not-a-route", NULL, NULL, 0);
    CHECK(stranger.status == 404 && nowhere.status == 404 && stranger.n == nowhere.n && memcmp(stranger.out, nowhere.out, stranger.n) == 0);
    r = call(&R, "POST", "/o", NULL, "hello", 0);
    CHECK(r.status == 404 && r.n == nowhere.n && memcmp(r.out, nowhere.out, r.n) == 0);
    r = call(&R, "GET", "/not-a-route", NULL, NULL, 1);
    CHECK(r.status == 404);
    r = call(&R, "DELETE", "/o", NULL, NULL, 1);
    CHECK(r.status == 405 && strstr(r.out, "Allow: GET, POST\r\n"));
    r = call(&R, "DELETE", "/o", NULL, NULL, 0);
    CHECK(r.status == 404 && r.n == nowhere.n && memcmp(r.out, nowhere.out, r.n) == 0); /* strangers get no 405 */
    r = call(&R, "DELETE", "/p", NULL, NULL, 1);
    CHECK(r.status == 404); /* peer routes are not the owner's to learn about, even with the token */
    r = call(&R, "GET", "/o", "v1 id=x", NULL, 0);
    CHECK(r.status == 404); /* a signature does not stand in for the token */
    r = call(&R, "POST", "/o", NULL, "01234567890", 1);
    CHECK(r.status == 404); /* over the route's body cap (the server never reads this, but if it did) */

    /* ---- body limits, asked before reading ---- */
    {
        puddle_request q;
        memset(&q, 0, sizeof q);
        strcpy(q.method, "POST"); strcpy(q.path, "/o");
        CHECK(puddle_router_body_limit(&R, &q, 1) == 10);
        CHECK(puddle_router_body_limit(&R, &q, 0) == 0);   /* a stranger's body to an owner route is refused unread */
        strcpy(q.path, "/p");
        CHECK(puddle_router_body_limit(&R, &q, 0) == 100); /* peers have no token; the signature comes after the body */
        CHECK(puddle_router_body_limit(&R, &q, 1) == 100);
        strcpy(q.path, "/c");
        CHECK(puddle_router_body_limit(&R, &q, 0) == 100);
        strcpy(q.path, "/nope");
        CHECK(puddle_router_body_limit(&R, &q, 1) == 0);
        strcpy(q.path, "/o"); strcpy(q.method, "GET");
        CHECK(puddle_router_body_limit(&R, &q, 1) == 0);
        strcpy(q.method, "DELETE");
        CHECK(puddle_router_body_limit(&R, &q, 1) == 0);
    }

    /* ---- peer routes ---- */
    sign(alice, "GET", "/p", NULL, bob_id, T0, sig, sizeof sig);
    r = call(&R, "GET", "/p", sig, NULL, 0);
    CHECK(r.status == 200 && strstr(body_of(&r), alice_id));
    r = call(&R, "GET", "/p", sig, NULL, 0);
    CHECK(r.status == 401 && strstr(body_of(&r), "\"replayed\""));      /* the same request again */
    r = call(&R, "GET", "/p", NULL, NULL, 1);
    CHECK(r.status == 404 && r.n == nowhere.n);                          /* no signature, even with the owner token */
    r = call(&R, "GET", "/p", "garbage", NULL, 0);
    CHECK(r.status == 404);

    sign(alice, "POST", "/p", "payload", bob_id, T0, sig, sizeof sig);
    r = call(&R, "POST", "/p", sig, "payload", 0);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"body\":7"));
    sign(alice, "POST", "/p", "payload", bob_id, T0 + 1, sig, sizeof sig);
    r = call(&R, "POST", "/p", sig, "payloaD", 0);
    CHECK(r.status == 404);                                              /* body changed after signing */
    sign(alice, "POST", "/p", "payload", bob_id, T0 + 2, sig, sizeof sig);
    r = call(&R, "POST", "/o", sig, "payload", 0);
    CHECK(r.status == 404);                                              /* signed for another route */
    sign(alice, "GET", "/p?x=1", NULL, bob_id, T0 + 3, sig, sizeof sig);
    r = call(&R, "GET", "/p?x=2", sig, NULL, 0);
    CHECK(r.status == 404);                                              /* query is signed too */
    {
        char carol_id[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(carol, carol_id);
        sign(alice, "GET", "/p", NULL, carol_id, T0 + 4, sig, sizeof sig);
        r = call(&R, "GET", "/p", sig, NULL, 0);
        CHECK(r.status == 404);                                          /* meant for someone else's Puddle */
        sign(carol, "GET", "/p", NULL, bob_id, T0 + 5, sig, sizeof sig);
        r = call(&R, "GET", "/p", sig, NULL, 0);
        CHECK(r.status == 404);                                          /* a signer we have no stream with */
    }
    sign(alice, "GET", "/p", NULL, bob_id, T0 - 10 * 60 * 1000, sig, sizeof sig);
    r = call(&R, "GET", "/p", sig, NULL, 0);
    CHECK(r.status == 401 && strstr(body_of(&r), "\"clock_skew\"") && strstr(body_of(&r), "1760000000000"));
    /* the signature had to pass first: a forged one is only ever a 404 */
    sign(carol, "GET", "/p", NULL, bob_id, T0 - 10 * 60 * 1000, sig, sizeof sig);
    r = call(&R, "GET", "/p", sig, NULL, 0);
    CHECK(r.status == 404);

    /* no streams known at all: every peer route rejects */
    {
        puddle_ctx none = ctx;
        none.peer_key = NULL;
        puddle_router R2 = { routes, sizeof routes / sizeof routes[0], &none };
        sign(alice, "GET", "/p", NULL, bob_id, T0 + 6, sig, sizeof sig);
        r = call(&R2, "GET", "/p", sig, NULL, 0);
        CHECK(r.status == 404);
    }

    /* ---- chain-proven routes ---- */
    sign(alice, "POST", "/c", "alice-chain", bob_id, T0 + 10, sig, sizeof sig);
    r = call(&R, "POST", "/c", sig, "alice-chain", 0);
    CHECK(r.status == 200 && strstr(body_of(&r), alice_id));
    r = call(&R, "POST", "/c", sig, "alice-chain", 0);
    CHECK(r.status == 401 && strstr(body_of(&r), "replayed"));           /* the one replay memory covers these too */
    sign(alice, "POST", "/c", "bogus-chain!", bob_id, T0 + 11, sig, sizeof sig);
    r = call(&R, "POST", "/c", sig, "bogus-chain!", 0);
    CHECK(r.status == 404);                                              /* the chain does not vouch for the key */
    sign(carol, "POST", "/c", "alice-chain", bob_id, T0 + 12, sig, sizeof sig);
    r = call(&R, "POST", "/c", sig, "alice-chain", 0);
    CHECK(r.status == 404);                                              /* someone else's signature on that chain */
    r = call(&R, "POST", "/c", NULL, "alice-chain", 1);
    CHECK(r.status == 404);                                              /* the token is not a chain proof */
    {
        puddle_route nokey = { "POST", "/c", PUDDLE_AUTH_CHAIN, 100, h_owner, NULL }; /* a chain route that forgot its lookup */
        puddle_router R3 = { &nokey, 1, &ctx };
        sign(alice, "POST", "/c", "alice-chain", bob_id, T0 + 13, sig, sizeof sig);
        r = call(&R3, "POST", "/c", sig, "alice-chain", 0);
        CHECK(r.status == 404);
    }

    /* ---- the replay memory fills: still just a refusal, and authenticated senders are told ---- */
    {
        puddle_ctx small = ctx;
        small.replay = puddle_replay_new(8);
        puddle_router R4 = { routes, sizeof routes / sizeof routes[0], &small };
        int busy = 0;
        for (int i = 0; i < 30; i++) {
            sign(alice, "GET", "/p", NULL, bob_id, T0 + 100 + i, sig, sizeof sig);
            r = call(&R4, "GET", "/p", sig, NULL, 0);
            if (r.status == 503) { busy = 1; CHECK(strstr(body_of(&r), "\"busy\"")); }
            else CHECK(r.status == 200);
        }
        CHECK(busy);
        puddle_replay_free(small.replay);
    }

    /* ---- peer routes with the real stream store behind them ---- */
    {
        char dd[700], e[300];
        snprintf(dd, sizeof dd, "%s/bob-data", root);
        puddle_streams *ss = puddle_streams_open(dd, e, sizeof e);
        CHECK(ss != NULL);
        puddle_ctx real = ctx;
        real.peer_key = puddle_streams_peer_key;
        real.peer_key_user = ss;
        puddle_router R5 = { routes, sizeof routes / sizeof routes[0], &real };
        int64_t t = T0 + 500;

        sign(alice, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        CHECK(call(&R5, "GET", "/p", sig, NULL, 0).status == 404); /* a stranger: no stream, no answer */

        puddle_stream rec;
        CHECK(puddle_stream_init(&rec, alice_id, PUDDLE_STREAM_PENDING_OUT, T0) == 0);
        CHECK(puddle_streams_put(ss, &rec, e, sizeof e) == 0);
        sign(alice, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        CHECK(call(&R5, "GET", "/p", sig, NULL, 0).status == 404); /* we asked, but hold no keys of hers yet */

        size_t cl;
        char *chain = puddle_identity_chain(alice, &cl);
        CHECK(puddle_stream_set_chain(&rec, chain, cl) == 0);
        free(chain);
        rec.state = PUDDLE_STREAM_ACTIVE;
        CHECK(puddle_streams_put(ss, &rec, e, sizeof e) == 0);
        sign(alice, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        r = call(&R5, "GET", "/p", sig, NULL, 0);
        CHECK(r.status == 200 && strstr(body_of(&r), alice_id));   /* a known peer, with her current key */

        sign(carol, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        CHECK(call(&R5, "GET", "/p", sig, NULL, 0).status == 404); /* a different person */

        CHECK(puddle_streams_block(ss, alice_id, t, e, sizeof e) == 0);
        sign(alice, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        CHECK(call(&R5, "GET", "/p", sig, NULL, 0).status == 404); /* blocked: as if she were not there */
        CHECK(puddle_streams_unblock(ss, alice_id, e, sizeof e) == 0);
        sign(alice, "GET", "/p", NULL, bob_id, t++, sig, sizeof sig);
        CHECK(call(&R5, "GET", "/p", sig, NULL, 0).status == 200);
        puddle_stream_free(&rec);
        puddle_streams_close(ss);
    }

    /* ---- answers: empty body, and a handler that overflows ---- */
    r = call(&R, "GET", "/empty", NULL, NULL, 1);
    CHECK(r.status == 202 && strstr(r.out, "202 Accepted") && strstr(r.out, "Content-Length: 0\r\n") && *body_of(&r) == '\0');
    r = call(&R, "GET", "/toolong", NULL, NULL, 1);
    CHECK(r.status == 500);

    /* ---- the Puddle's real table: /health ---- */
    puddle_router real = { puddle_routes, puddle_routes_count, &ctx };
    r = call(&real, "GET", "/health", NULL, NULL, 1);
    CHECK(r.status == 200 && strstr(r.out, "200 OK") && strcmp(body_of(&r), "{\"status\":\"ok\"}") == 0);
    CHECK(strstr(r.out, "Content-Length: 15\r\n") != NULL);
    r = call(&real, "GET", "/health?verbose=1", NULL, NULL, 1);
    CHECK(r.status == 200);
    r = call(&real, "GET", "/health", NULL, NULL, 0);
    CHECK(r.status == 404 && r.n == nowhere.n && memcmp(r.out, nowhere.out, r.n) == 0);
    r = call(&real, "POST", "/health", NULL, NULL, 1);
    CHECK(r.status == 405 && strstr(r.out, "Allow: GET\r\n"));
    r = call(&real, "HEAD", "/health", NULL, NULL, 1);
    CHECK(r.status == 405); /* the APIs are GET and POST only */
    r = call(&real, "POST", "/health", NULL, NULL, 0);
    CHECK(r.status == 404);

    /* ---- query strings ---- */
    {
        const char *keys[] = { "limit", "state" };
        char v[2][PUDDLE_QUERY_VALUE_MAX];
        CHECK(puddle_query_parse("/x", keys, 2, v) == 0 && !v[0][0] && !v[1][0]);
        CHECK(puddle_query_parse("/x?limit=5&state=active", keys, 2, v) == 0 && !strcmp(v[0], "5") && !strcmp(v[1], "active"));
        CHECK(puddle_query_parse("/x?state=pending_in", keys, 2, v) == 0 && !strcmp(v[1], "pending_in") && !v[0][0]);
        CHECK(puddle_query_parse("/x?state=a-B_9", keys, 2, v) == 0);
        CHECK(puddle_query_parse("/x?state=0123456789012345678901234567890", keys, 2, v) == 0 && strlen(v[1]) == 31);
        char q79[300], q80[300];
        snprintf(q79, sizeof q79, "/x?state=%.79s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        snprintf(q80, sizeof q80, "/x?state=%.80s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK(puddle_query_parse(q79, keys, 2, v) == 0 && strlen(v[1]) == 79 && puddle_query_parse(q80, keys, 2, v) == -1);
        const char *bad[] = { "/x?state=a%20b", "/x?state=a b", "/x?state=a.b", "/x?state=a/b",
                              "/x?other=1", "/x?limit=1&limit=2", "/x?limit=", "/x?limit", "/x?=1", "/x?&", "/x?limit=1&", "/x?&limit=1",
                              "/x?limit=1&&state=a", "/x?limit==1", "/x?LIMIT=1", "/x?limit=1;state=a", "/x?state=\xC3\xA9", "/x?limit=1#f" };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            if (puddle_query_parse(bad[i], keys, 2, v) == 0) { fprintf(stderr, "FAIL accepted query: %s\n", bad[i]); failures++; }
        char big[200];
        memset(big, 'a', 150);
        strcpy(big + 150, "");
        char t[400];
        snprintf(t, sizeof t, "/x?state=%s", big);
        CHECK(puddle_query_parse(t, keys, 2, v) == -1);                    /* longer than the room: refused, not truncated */
    }

    puddle_replay_free(ctx.replay);
    puddle_identity_close(alice); puddle_identity_close(bob); puddle_identity_close(carol);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
