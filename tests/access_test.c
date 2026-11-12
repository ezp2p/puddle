#include "federation/access.h"
#include "core/people.h"

#include "base/text.h"
#include "base/encoding.h"
#include "core/phrase.h"
#include "service/routes.h"
#include "federation/courier.h"

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
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

typedef struct {
    puddle_identity *id;
    char uid[PUDDLE_USER_ID_LEN + 1];
} person;

static void make(person *p, const char *name)
{
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!p->id) { fprintf(stderr, "cannot make %s: %s\n", name, err); exit(2); }
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

static person bob, alice, carol, dave;
static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
static puddle_people *ps;

/* the profile their Puddle sent us, or "" */
static const char *their_profile(const char *id)
{
    static char buf[20000];
    char *json;
    size_t len;
    buf[0] = '\0';
    if (puddle_people_profile(ps, id, PUDDLE_PEOPLE_DM, &json, &len) == 0) {
        snprintf(buf, sizeof buf, "%s", json);
        free(json);
    }
    return buf;
}

typedef struct { int status; char out[2048]; size_t n; } reply;

static reply post(const char *body, const char *sig)
{
    puddle_request req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "POST");
    strcpy(req.path, "/stream/v1/access");
    strcpy(req.target, "/stream/v1/access");
    if (sig) snprintf(req.signature, sizeof req.signature, "%s", sig);
    size_t bl = strlen(body);
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    memcpy(copy, body, bl + 1);
    reply rp;
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, 0, copy, bl, rp.out, sizeof rp.out, &rp.status);
    free(copy);
    return rp;
}

/* a request body from `who`; `extra` goes inside the top-level object (after "requester") */
static char *body_for(person *who, const char *profile, const char *address, const char *extra, const char *chain_override)
{
    size_t cl;
    char *chain = puddle_identity_chain(who->id, &cl);
    size_t cap = cl * 6 + 4096;
    char *buf = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "requester");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", who->uid);
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, profile, strlen(profile));
    puddle_jw_str(&w, "address", address);
    puddle_jw_str(&w, "chain", chain_override ? chain_override : chain);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    free(chain);
    size_t n = puddle_jw_finish(&w);
    if (!n) { fprintf(stderr, "body too big\n"); exit(2); }
    if (extra && *extra) { /* splice: ..."}}  ->  ..."},<extra>} */
        buf[n - 1] = ',';
        strcpy(buf + n, extra);
        strcat(buf, "}");
    }
    return buf;
}

static void sign_for(person *who, const char *body, char *sig, size_t cap)
{
    if (puddle_reqsig_sign(who->id, "POST", "/stream/v1/access", body, strlen(body), bob.uid, clock_now, sig, cap) != 0) {
        fprintf(stderr, "sign failed\n"); exit(2);
    }
}

/* send a request from `who` and return the HTTP status */
static int ask(person *who, const char *profile, const char *extra, reply *out)
{
    char *b = body_for(who, profile, "203.0.113.7:8443", extra, NULL);
    char sig[700];
    sign_for(who, b, sig, sizeof sig);
    reply r = post(b, sig);
    free(b);
    if (out) *out = r;
    return r.status;
}

#define PROFILE "{\"display_name\":\"Alice\"}"

static int state_of(person *p, puddle_stream_state *st)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, p->uid, &rec) != 0) return 0;
    *st = rec.state;
    puddle_stream_free(&rec);
    return 1;
}
static int owes(person *p, puddle_owed want)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, p->uid, &rec) != 0) return 0;
    int ok = rec.owed == want;
    puddle_stream_free(&rec);
    return ok;
}
static int has(person *p) { puddle_stream_state s; return state_of(p, &s); }
static int is(person *p, puddle_stream_state want) { puddle_stream_state s; return state_of(p, &s) && s == want; }

static void forget(person *p)
{
    char path[900];
    snprintf(path, sizeof path, "%s/bob-data/state/streams/%s.json", root, p->uid);
    remove(path);
}

static void reopen(void)
{
    char err[200], dd[700];
    puddle_streams_close(ss);
    snprintf(dd, sizeof dd, "%s/bob-data", root);
    ss = puddle_streams_open(dd, err, sizeof err);
    if (!ps) ps = puddle_people_open(dd, err, sizeof err);
    ctx.streams = ss;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-access-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&bob, "bob"); make(&alice, "alice"); make(&carol, "carol"); make(&dave, "dave");

    char err[300], dd[700];
    snprintf(dd, sizeof dd, "%s/bob-data", root);
    ss = puddle_streams_open(dd, err, sizeof err);
    if (!ps) ps = puddle_people_open(dd, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .identity = bob.id, .streams = ss, .people = ps, .replay = puddle_replay_new(1024), .now_ms = now_cb,
                        .anon_bucket = puddle_bucket_new(50, 100), .access_limit = puddle_keyed_new(64, 1000, 3600 * 1000LL) };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);

    reply r, first;
    puddle_stream rec;

    /* ---- a request, the default policy: it waits for the owner ---- */
    CHECK(puddle_streams_access(ss) == PUDDLE_ACCESS_MANUAL);
    CHECK(ask(&alice, "{\"display_name\":\"Alice\",\"blurb\":\"hi\",\"evil\":{\"x\":1},\"color\":{\"colors\":[\"#AABBCC\",\"#001122\"],\"angle\":90},"
                  "\"avatar\":{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mime\":\"image/png\",\"size\":1234}}", "\"message\":\"hey it's Alice\\nfrom work\"", &first) == 202);
    CHECK(first.n > 0 && strstr(first.out, "202 Accepted") && strstr(first.out, "Content-Length: 0\r\n"));
    CHECK(strcmp(strstr(first.out, "\r\n\r\n") + 4, "") == 0);
    CHECK(is(&alice, PUDDLE_STREAM_PENDING_IN) && owes(&alice, PUDDLE_OWED_NONE));
    CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0);
    CHECK(!strcmp(rec.note, "hey it's Alice\nfrom work") && rec.naddr == 1 && !strcmp(rec.addresses[0], "203.0.113.7:8443"));
    CHECK(rec.chain && rec.chain_len > 0 && rec.created_ms == T0);
    CHECK(!strcmp(their_profile(alice.uid), "{\"display_name\":\"Alice\",\"blurb\":\"hi\",\"color\":{\"colors\":[\"#aabbcc\",\"#001122\"],\"angle\":90},"
                                              "\"avatar\":{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mime\":\"image/png\",\"size\":1234}}"));
    /* ...rebuilt from the fields we know: the unknown one is gone, the color is in one spelling */
    puddle_stream_free(&rec);
    {
        unsigned char pub[32];
        char kid[65];
        puddle_identity_public(alice.id, pub, NULL, NULL);
        puddle_key_id(pub, kid);
        unsigned char got[32];
        CHECK(puddle_streams_peer_key(ss, alice.uid, kid, got) == 1); /* the first time we hold her keys */
    }

    /* replaying the very same request */
    {
        char *b = body_for(&alice, PROFILE, "203.0.113.7:8443", NULL, NULL);
        char sig[700];
        sign_for(&alice, b, sig, sizeof sig);
        CHECK(post(b, sig).status == 202);
        r = post(b, sig);
        CHECK(r.status == 401 && strstr(r.out, "replayed"));
        free(b);
    }

    /* ---- anything that does not authenticate is a 404, the same as an unknown path ---- */
    {
        reply nothing = { 0 };
        puddle_request q;
        memset(&q, 0, sizeof q);
        strcpy(q.method, "GET"); strcpy(q.path, "/nope"); strcpy(q.target, "/nope");
        nothing.n = puddle_router_handle(&R, &q, 0, (const unsigned char *)"", 0, nothing.out, sizeof nothing.out, &nothing.status);

        char *good = body_for(&carol, PROFILE, "203.0.113.7:8443", NULL, NULL);
        char sig[700], sig2[700];
        sign_for(&carol, good, sig, sizeof sig);

        r = post(good, NULL);
        CHECK(r.status == 404 && r.n == nothing.n && !memcmp(r.out, nothing.out, r.n));        /* no signature */
        r = post(good, "v1 id=x");
        CHECK(r.status == 404 && r.n == nothing.n);                                              /* garbage signature */
        char *tampered = strdup(good);
        char *p = strstr(tampered, "203.0.113.7");
        p[2] = '4';
        r = post(tampered, sig);
        CHECK(r.status == 404 && r.n == nothing.n);                                              /* body changed after signing */
        free(tampered);
        CHECK(!has(&carol));

        /* signed by Dave, claiming to be Carol */
        sign_for(&dave, good, sig2, sizeof sig2);
        CHECK(post(good, sig2).status == 404);
        /* Carol's body but Dave's chain: the chain does not belong to the id */
        size_t dl;
        char *dchain = puddle_identity_chain(dave.id, &dl);
        char *mismatch = body_for(&carol, PROFILE, "203.0.113.7:8443", NULL, dchain);
        free(dchain);
        sign_for(&carol, mismatch, sig, sizeof sig);
        CHECK(post(mismatch, sig).status == 404);
        sign_for(&dave, mismatch, sig, sizeof sig);
        CHECK(post(mismatch, sig).status == 404);
        free(mismatch);
        /* a request meant for someone else's Puddle */
        char sigx[700];
        puddle_reqsig_sign(carol.id, "POST", "/stream/v1/access", good, strlen(good), alice.uid, clock_now, sigx, sizeof sigx);
        CHECK(post(good, sigx).status == 404);
        free(good);

        /* malformed in every way a request can be */
        /* malformed: where they say they are, or what they say, cannot be used */
        struct { const char *profile, *addr, *extra; } bad[] = {
            { "{\"display_name\":\"Carol\"}", "not an address", NULL },
            { "{\"display_name\":\"Carol\"}", "", NULL },
            { "{\"display_name\":\"Carol\"}", "0.0.0.0:8443", NULL },                    /* somewhere we would never connect */
            { "{\"display_name\":\"Carol\"}", "169.254.169.254:80", NULL },
            { "{\"display_name\":\"Carol\"}", "[fe80::1]:8443", NULL },
            { "{\"display_name\":\"Carol\"}", "203.0.113.7:8443", "\"message\":7" },
            { "{\"display_name\":\"Carol\"}", "203.0.113.7:8443", "\"message\":\"x\",\"message\":\"y\"" },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char *b = body_for(&carol, bad[i].profile, bad[i].addr, bad[i].extra, NULL);
            sign_for(&carol, b, sig, sizeof sig);
            r = post(b, sig);
            if (r.status != 404) { fprintf(stderr, "FAIL malformed #%zu answered %d\n", i, r.status); failures++; }
            free(b);
        }
        /* a profile with something wrong in it does not make the request malformed: what is wrong is left out, with a note */
        struct { const char *profile, *kept, *noted; } lenient[] = {
            { "{}", "{}", "" },                                                                 /* no display name: shown by their id */
            { "{\"display_name\":\"\"}", "{}", "" },
            { "{\"display_name\":\"  \\u202e  \"}", "{}", "" },                            /* nothing left once cleaned */
            { "{\"display_name\":7}", "{}", "display_name: not text" },
            { "[1]", "", "profile: not something we can read" },
            { "{\"display_name\":\"Carol\",\"avatar\":{\"hash\":\"zz\",\"mime\":\"image/png\",\"size\":5}}", "{\"display_name\":\"Carol\"}", "avatar:" },
            { "{\"display_name\":\"Carol\",\"avatar\":{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mime\":\"image/svg+xml\",\"size\":5}}", "{\"display_name\":\"Carol\"}", "avatar:" },
            { "{\"display_name\":\"Carol\",\"banner\":\"nope\"}", "{\"display_name\":\"Carol\"}", "banner:" },
            { "{\"display_name\":\"Carol\",\"color\":{\"colors\":[\"red\"]}}", "{\"display_name\":\"Carol\"}", "color: \"red\" is not a colour" },
            { "{\"display_name\":\"Carol\",\"color\":\"#000000\"}", "{\"display_name\":\"Carol\"}", "color: not a list" },
            { "{\"display_name\":\"Carol\",\"blurb\":7}", "{\"display_name\":\"Carol\"}", "blurb: not text" },
            { "{\"display_name\":\"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\"}",
              "{\"display_name\":\"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\"}", "cut short" },     /* 65 characters, cut to 64 */
        };
        for (size_t i = 0; i < sizeof lenient / sizeof lenient[0]; i++) {
            char obj[1000];
            snprintf(obj, sizeof obj, "{\"id\":\"%s\",\"address\":\"203.0.113.7:8443\",\"profile\":%s,\"chain\":\"x\"}", carol.uid, lenient[i].profile);
            puddle_peer_info pi;
            int ok = puddle_peer_parse(obj, strlen(obj), &pi) == 0 && !strcmp(pi.profile, lenient[i].kept) &&
                     (lenient[i].noted[0] ? strstr(pi.profile_notes, lenient[i].noted) != NULL : pi.profile_notes[0] == '\0');
            if (!ok) { fprintf(stderr, "FAIL lenient #%zu: kept %s, noted [%s]\n", i, pi.profile, pi.profile_notes); failures++; }
            puddle_peer_info_free(&pi);
        }
        {
            /* and none at all: they show us nothing, which is theirs to choose */
            char obj[300];
            snprintf(obj, sizeof obj, "{\"id\":\"%s\",\"address\":\"203.0.113.7:8443\",\"chain\":\"x\"}", carol.uid);
            puddle_peer_info pi;
            CHECK(puddle_peer_parse(obj, strlen(obj), &pi) == 0 && pi.profile[0] == '\0' && pi.profile_notes[0] == '\0');
            puddle_peer_info_free(&pi);
        }
        char note[2200];
        strcpy(note, "\"message\":\"");
        memset(note + 11, 'm', 501); strcpy(note + 512, "\"");
        char *b = body_for(&carol, PROFILE, "203.0.113.7:8443", note, NULL);
        sign_for(&carol, b, sig, sizeof sig);
        CHECK(post(b, sig).status == 404);                                                       /* a 501-character note */
        free(b);
        b = body_for(&carol, PROFILE, "203.0.113.7:8443", "\"scope\":{\"group\":\"x\"}", NULL);
        sign_for(&carol, b, sig, sizeof sig);
        CHECK(post(b, sig).status == 404 && !has(&carol));                                       /* a group-only request with no proof in it is not one */
        free(b);
        CHECK(post("not json", NULL).status == 404 && post("{}", NULL).status == 404);
        CHECK(!has(&carol));
    }

    /* ---- what the owner sees is cleaned ---- */
    {
        char *b = body_for(&carol, "{\"display_name\":\"  Car\\u202eol\\u0007 \\u200f\\n\"}", "[2001:DB8::1]:9000",
                           "\"message\":\"  line1\\tx\\r\\nline2\\u0001\\u2028end  \"", NULL);
        char sig[700];
        sign_for(&carol, b, sig, sizeof sig);
        CHECK(post(b, sig).status == 202);
        free(b);
        CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0);
        CHECK(!strcmp(their_profile(carol.uid), "{\"display_name\":\"Carol\"}"));
        CHECK(!strcmp(rec.note, "line1 x \nline2 end"));
        CHECK(!strcmp(rec.addresses[0], "[2001:db8::1]:9000"));
        puddle_stream_free(&rec);
        forget(&carol);
        reopen();
    }

    /* ---- the same answer, whatever was decided ---- */
    clock_now += 1000;
    CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_OPEN, err, sizeof err) == 0);
    CHECK(ask(&dave, PROFILE, NULL, &r) == 202);
    CHECK(is(&dave, PUDDLE_STREAM_ACTIVE) && owes(&dave, PUDDLE_OWED_APPROVED));                 /* open: let in at once, and owed the news */
    CHECK(r.n == first.n && !memcmp(r.out, first.out, r.n));                                     /* same bytes as "waiting" */
    CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_CLOSED, err, sizeof err) == 0);
    CHECK(ask(&carol, PROFILE, NULL, &r) == 202 && !has(&carol));                                /* closed: ignored */
    CHECK(r.n == first.n && !memcmp(r.out, first.out, r.n));
    CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_MANUAL, err, sizeof err) == 0);
    CHECK(puddle_streams_block(ss, carol.uid, clock_now, err, sizeof err) == 0);
    CHECK(ask(&carol, PROFILE, NULL, &r) == 202 && !has(&carol));                                /* blocked: ignored */
    CHECK(r.n == first.n && !memcmp(r.out, first.out, r.n));
    CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);

    /* ---- invites ---- */
    {
        puddle_invite inv;
        CHECK(puddle_streams_invite_create(ss, clock_now, 3600 * 1000, 1, &inv, err, sizeof err) == 0);
        char extra[100];
        snprintf(extra, sizeof extra, "\"invite\":\"%s\"", inv.token);
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_CLOSED, err, sizeof err) == 0);
        CHECK(ask(&carol, PROFILE, extra, NULL) == 202 && !has(&carol));                         /* closed beats an invite */
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_OPEN, err, sizeof err) == 0);
        CHECK(ask(&carol, PROFILE, extra, NULL) == 202 && is(&carol, PUDDLE_STREAM_ACTIVE));
        forget(&carol); reopen();
        /* neither of those spent it */
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_MANUAL, err, sizeof err) == 0);
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, extra, NULL) == 202 && is(&carol, PUDDLE_STREAM_ACTIVE));     /* manual + a good invite */
        forget(&carol); reopen();
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, extra, NULL) == 202 && is(&carol, PUDDLE_STREAM_PENDING_IN)); /* one use only: now it waits */
        forget(&carol); reopen();
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, "\"invite\":\"abcdefghijklmnopqrstuvwxy2\"", NULL) == 202);   /* a made-up token */
        CHECK(is(&carol, PUDDLE_STREAM_PENDING_IN));
        forget(&carol); reopen();
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, "\"invite\":\"junk\"", NULL) == 202 && is(&carol, PUDDLE_STREAM_PENDING_IN)); /* junk is no token */
        forget(&carol); reopen();
    }

    /* a blocked requester with a good invite: ignored, and the invite is not spent on them */
    {
        puddle_invite inv;
        char extra[100];
        CHECK(puddle_streams_invite_create(ss, clock_now, 3600 * 1000, 1, &inv, err, sizeof err) == 0);
        snprintf(extra, sizeof extra, "\"invite\":\"%s\"", inv.token);
        CHECK(puddle_streams_block(ss, carol.uid, clock_now, err, sizeof err) == 0);
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, extra, NULL) == 202 && !is(&carol, PUDDLE_STREAM_ACTIVE) && !is(&carol, PUDDLE_STREAM_PENDING_IN));
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        CHECK(puddle_streams_invite_consume(ss, inv.token, clock_now) == 1); /* still there */
        forget(&carol); reopen();
    }
    /* signed by one person's key but naming another in the header: nothing is recorded for either */
    {
        char *b = body_for(&carol, PROFILE, "203.0.113.7:8443", NULL, NULL);
        char sig[700], forged[800];
        sign_for(&carol, b, sig, sizeof sig);
        char *at = strstr(sig, carol.uid);
        CHECK(at != NULL);
        memcpy(forged, sig, sizeof sig);
        if (at) memcpy(forged + (at - sig), dave.uid, PUDDLE_USER_ID_LEN);
        size_t before = puddle_streams_size(ss);
        int s = post(b, forged).status;
        CHECK((s == 202 || s == 404) && puddle_streams_size(ss) == before && !has(&carol));
        free(b);
    }
    /* a Puddle with room for only so many people */
    {
        size_t had = puddle_streams_size(ss);
        ctx.max_peers = had;
        forget(&carol);
        reopen();
        ctx.max_peers = puddle_streams_size(ss);
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, NULL, NULL) == 202 && !has(&carol));         /* full: a stranger is ignored */
        CHECK(ask(&alice, PROFILE, "\"message\":\"known\"", NULL) == 202);       /* someone on file still can */
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && !strcmp(rec.note, "known"));
        puddle_stream_free(&rec);
        ctx.max_peers = 0;
    }

    /* ---- people we already know ---- */
    clock_now += 1000;
    CHECK(ask(&alice, PROFILE, "\"message\":\"again\"", NULL) == 202);                            /* still waiting: refreshed */
    CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && rec.state == PUDDLE_STREAM_PENDING_IN && !strcmp(rec.note, "again") && rec.created_ms == T0);
    puddle_stream_free(&rec);
    CHECK(puddle_streams_get(ss, dave.uid, &rec) == 0);
    snprintf(rec.note, sizeof rec.note, "original");
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
    clock_now += 1000;
    CHECK(ask(&dave, PROFILE, "\"message\":\"changed\"", NULL) == 202);                           /* already a stream: left alone */
    CHECK(puddle_streams_get(ss, dave.uid, &rec) == 0 && rec.state == PUDDLE_STREAM_ACTIVE && !strcmp(rec.note, "original"));
    rec.state = PUDDLE_STREAM_CLOSED;
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
    clock_now += 1000;
    CHECK(ask(&dave, PROFILE, "\"message\":\"back\"", NULL) == 202 && is(&dave, PUDDLE_STREAM_PENDING_IN)); /* closed: may ask again */

    /* we asked them and they ask us: nobody needs to approve anything, even when "closed" */
    CHECK(puddle_stream_init(&rec, carol.uid, PUDDLE_STREAM_PENDING_OUT, clock_now) == 0);
    CHECK(puddle_stream_add_address(&rec, "198.51.100.9:8443") == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
    CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_CLOSED, err, sizeof err) == 0);
    clock_now += 1000;
    CHECK(ask(&carol, PROFILE, NULL, NULL) == 202 && is(&carol, PUDDLE_STREAM_ACTIVE) && owes(&carol, PUDDLE_OWED_APPROVED));
    CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0 && rec.chain && rec.naddr == 1 && !strcmp(rec.addresses[0], "203.0.113.7:8443"));
    puddle_stream_free(&rec);
    CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_MANUAL, err, sizeof err) == 0);

    /* ---- limits ---- */
    {
        /* five an hour per requester */
        forget(&carol); reopen();
        puddle_keyed_free(ctx.access_limit);
        ctx.access_limit = puddle_keyed_new(64, 5, 3600 * 1000LL);
        clock_now += 5000;
        int64_t start = clock_now;
        for (int i = 0; i < 5; i++) { clock_now = start + i * 1000; CHECK(ask(&carol, PROFILE, "\"message\":\"n\"", NULL) == 202); }
        CHECK(is(&carol, PUDDLE_STREAM_PENDING_IN));
        puddle_stream_free(&rec);
        CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0);
        snprintf(rec.note, sizeof rec.note, "marker");
        CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
        puddle_stream_free(&rec);
        clock_now = start + 6000;
        CHECK(ask(&carol, PROFILE, "\"message\":\"sixth\"", NULL) == 202);
        CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0 && !strcmp(rec.note, "marker"));    /* dropped: nothing changed */
        puddle_stream_free(&rec);
        clock_now = start + 3600 * 1000 + 1;
        CHECK(ask(&carol, PROFILE, "\"message\":\"later\"", NULL) == 202);
        CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0 && !strcmp(rec.note, "later"));
        puddle_stream_free(&rec);

        /* too many waiting: new strangers are dropped, people already waiting can still update */
        forget(&carol); reopen();
        puddle_stream_state st;
        (void)st;
        for (size_t i = has(&alice) + 0; puddle_streams_count(ss, PUDDLE_STREAM_PENDING_IN) < PUDDLE_MAX_PENDING_IN; i++) {
            unsigned char raw[32];
            memset(raw, 0, sizeof raw);
            raw[0] = (unsigned char)(i & 255); raw[1] = (unsigned char)(i >> 8); raw[31] = 0x00;
            char id[64];
            puddle_base32_encode(raw, 32, id, sizeof id);
            puddle_stream f;
            CHECK(puddle_stream_init(&f, id, PUDDLE_STREAM_PENDING_IN, clock_now) == 0);
            CHECK(puddle_streams_put(ss, &f, err, sizeof err) == 0);
            puddle_stream_free(&f);
        }
        CHECK(puddle_streams_count(ss, PUDDLE_STREAM_PENDING_IN) == PUDDLE_MAX_PENDING_IN);
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, NULL, NULL) == 202 && !has(&carol));
        CHECK(ask(&alice, PROFILE, "\"message\":\"still here\"", NULL) == 202);
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && !strcmp(rec.note, "still here"));
        puddle_stream_free(&rec);
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_OPEN, err, sizeof err) == 0);
        clock_now += 1000;
        CHECK(ask(&carol, PROFILE, NULL, NULL) == 202 && is(&carol, PUDDLE_STREAM_ACTIVE));    /* approving adds nothing to the queue */
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_MANUAL, err, sizeof err) == 0);

        /* checking chains for strangers is limited as a whole */
        puddle_bucket_free(ctx.anon_bucket);
        ctx.anon_bucket = puddle_bucket_new(1, 3);
        int ok = 0, refused = 0;
        for (int i = 0; i < 8; i++) {
            clock_now += 10;
            char *b = body_for(&dave, PROFILE, "203.0.113.7:8443", NULL, NULL);
            char sig[700];
            sign_for(&dave, b, sig, sizeof sig);
            int s = post(b, sig).status;
            if (s == 202) ok++; else if (s == 404) refused++;
            free(b);
        }
        CHECK(ok == 3 && refused == 5);
    }

    /* ---- the text cleaner ---- */
    {
        char out[200];
        size_t n;
        CHECK(puddle_clean_text("  hello  ", 9, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "hello") && n == 5);
        CHECK(puddle_clean_text("a\nb", 3, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "a b"));
        CHECK(puddle_clean_text("a\nb", 3, 10, 1, out, sizeof out, &n) == 0 && !strcmp(out, "a\nb"));
        CHECK(puddle_clean_text("\n\nx\n\n", 5, 10, 1, out, sizeof out, &n) == 0 && !strcmp(out, "x") && n == 1);
        CHECK(puddle_clean_text("a\x01\x7f\xC2\x85z", 6, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "az"));
        CHECK(puddle_clean_text("a\xE2\x80\xAE" "b\xE2\x81\xA6" "c", 9, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "abc")); /* RLO, LRI */
        CHECK(puddle_clean_text("x\xFF" "y", 3, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "x\xEF\xBF\xBD" "y") && n == 3);
        CHECK(puddle_clean_text("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x99\x82", 9, 3, 0, out, sizeof out, &n) == 0 && n == 3); /* three characters */
        CHECK(puddle_clean_text("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x99\x82", 9, 2, 0, out, sizeof out, &n) == -1);
        CHECK(puddle_clean_text("abcdef", 6, 5, 0, out, sizeof out, &n) == -1);
        CHECK(puddle_clean_text("abcdef", 6, 10, 0, out, 6, &n) == -1);            /* no room for the NUL */
        CHECK(puddle_clean_text("abcde", 5, 10, 0, out, 6, &n) == 0);
        CHECK(puddle_clean_text("", 0, 10, 0, out, sizeof out, &n) == 0 && !*out && n == 0);
        CHECK(puddle_clean_text("   ", 3, 10, 0, out, sizeof out, &n) == 0 && !*out && n == 0);
        CHECK(puddle_clean_text("a   ", 4, 1, 0, out, sizeof out, &n) == 0 && n == 1);  /* trailing blanks don't count against the limit */
        CHECK(puddle_clean_text("a \xE2\x80\xAE ", 6, 10, 0, out, sizeof out, &n) == 0 && !strcmp(out, "a"));
        char many[400];
        memset(many, ' ', sizeof many);
        many[399] = 'x';
        CHECK(puddle_clean_text(many, sizeof many, 5, 0, out, sizeof out, &n) == 0 && !strcmp(out, "x"));
    }

    puddle_people_close(ps);
    puddle_streams_close(ss);
    puddle_replay_free(ctx.replay);
    puddle_bucket_free(ctx.anon_bucket);
    puddle_keyed_free(ctx.access_limit);
    puddle_identity_close(bob.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id); puddle_identity_close(dave.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
