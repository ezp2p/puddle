#include "federation/access.h"
#include "core/people.h"
#include "federation/flow.h"
#include "federation/inbound.h"

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

typedef struct { puddle_identity *id; char uid[PUDDLE_USER_ID_LEN + 1]; } person;
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

static void make(person *p, const char *name)
{
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!p->id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

typedef struct { int status; char out[4096]; size_t n; } reply;

static reply send_raw(const char *path, const char *body, const char *sig)
{
    puddle_request req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "POST");
    snprintf(req.path, sizeof req.path, "%s", path);
    snprintf(req.target, sizeof req.target, "%s", path);
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

static const char *body_of(const reply *r) { const char *b = strstr(r->out, "\r\n\r\n"); return b ? b + 4 : ""; }

/* {"<key>":{id,address,profile,chain}<extra>} */
static char *peer_body(person *who, const char *as_id, const char *key, const char *profile, const char *extra)
{
    size_t cl;
    char *chain = puddle_identity_chain(who->id, &cl);
    size_t cap = cl * 6 + 2048;
    char *buf = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_key(&w, key);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", as_id);
    puddle_jw_str(&w, "address", "198.51.100.5:8443");
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, profile, strlen(profile));
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    free(chain);
    size_t n = puddle_jw_finish(&w);
    if (extra && *extra) { buf[n - 1] = ','; strcpy(buf + n, extra); strcat(buf, "}"); }
    return buf;
}

static reply signed_post(person *who, const char *path, const char *body)
{
    char sig[800];
    if (puddle_reqsig_sign(who->id, "POST", path, body, strlen(body), bob.uid, clock_now, sig, sizeof sig) != 0) exit(2);
    return send_raw(path, body, sig);
}

static void set_record(person *p, puddle_stream_state st, puddle_owed owed, int with_chain)
{
    puddle_stream rec;
    char err[300];
    CHECK(puddle_stream_init(&rec, p->uid, st, T0) == 0);
    rec.owed = owed;
    CHECK(puddle_stream_add_address(&rec, "203.0.113.9:8443") == 0);
    if (with_chain) {
        size_t cl;
        char *chain = puddle_identity_chain(p->id, &cl);
        CHECK(puddle_stream_set_chain(&rec, chain, cl) == 0);
        free(chain);
    }
    snprintf(rec.error, sizeof rec.error, "old trouble");
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

static int state_is(person *p, puddle_stream_state want)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, p->uid, &rec) != 0) return 0;
    int ok = rec.state == want;
    puddle_stream_free(&rec);
    return ok;
}
static int has(person *p) { puddle_stream rec; if (puddle_streams_get(ss, p->uid, &rec) != 0) return 0; puddle_stream_free(&rec); return 1; }

#define OKPROFILE "{\"display_name\":\"Them\",\"blurb\":\"hello\"}"

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-inbound-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&bob, "bob"); make(&alice, "alice"); make(&carol, "carol"); make(&dave, "dave");
    char err[300], dd[700];
    snprintf(dd, sizeof dd, "%s/bob-data", root);
    ss = puddle_streams_open(dd, err, sizeof err);
    if (!ps) ps = puddle_people_open(dd, err, sizeof err);
    puddle_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", dd);
    puddle_profiles *profiles;
    char old_default[PUDDLE_PROFILE_ID_MAX + 1];
    if (!(profiles = puddle_profiles_open(&cfg, old_default, err, sizeof err))) { fprintf(stderr, "profiles: %s\n", err); return 2; }
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .profiles = profiles, .identity = bob.id, .streams = ss, .people = ps, .replay = puddle_replay_new(1024), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply r;
    puddle_stream rec;
    char *b;

    /* ---- the answer to a request we made ---- */
    set_record(&alice, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);
    b = peer_body(&alice, alice.uid, "responder", OKPROFILE, "\"approved\":true");
    r = signed_post(&alice, "/stream/v1/access/result", b);
    CHECK(r.status == 202 && strstr(r.out, "Content-Length: 0\r\n"));
    CHECK(state_is(&alice, PUDDLE_STREAM_ACTIVE));
    CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0);
    CHECK(rec.chain && rec.chain_len > 0 && strstr(their_profile(alice.uid), "Them") && !rec.error[0] && rec.updated_ms == T0 + 0);
    CHECK(rec.naddr == 2 && !strcmp(rec.addresses[0], "203.0.113.9:8443") && !strcmp(rec.addresses[1], "198.51.100.5:8443")); /* the link's first */
    puddle_stream_free(&rec);
    {
        unsigned char pub[32], got[32];
        char kid[65];
        puddle_identity_public(alice.id, pub, NULL, NULL);
        puddle_key_id(pub, kid);
        CHECK(puddle_streams_peer_key(ss, alice.uid, kid, got) == 1);                          /* her keys are now ours to check against */
    }
    free(b);

    /* declined: closed, and nothing of hers kept */
    set_record(&carol, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);
    b = peer_body(&carol, carol.uid, "responder", OKPROFILE, "\"approved\":false");
    r = signed_post(&carol, "/stream/v1/access/result", b);
    CHECK(r.status == 202 && state_is(&carol, PUDDLE_STREAM_CLOSED));
    CHECK(puddle_streams_get(ss, carol.uid, &rec) == 0 && !rec.chain && !rec.error[0]);
    puddle_stream_free(&rec);
    free(b);

    /* answers that change nothing */
    {
        static const char *junk[] = { "\"approved\":\"yes\"", "\"approved\":1", "\"approved\":null", "\"approved\":\"true\"", "\"other\":1" };
        for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
            set_record(&dave, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);
            b = peer_body(&dave, dave.uid, "responder", OKPROFILE, junk[i]);
            r = signed_post(&dave, "/stream/v1/access/result", b);
            if (r.status != 202 || !state_is(&dave, PUDDLE_STREAM_PENDING_OUT)) { fprintf(stderr, "FAIL junk answer %zu was acted on\n", i); failures++; }
            free(b);
        }
        b = peer_body(&dave, dave.uid, "responder", OKPROFILE, NULL); /* no verdict at all */
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&dave, PUDDLE_STREAM_PENDING_OUT));
        free(b);
        /* not a request of ours: whatever state we are in with them, an answer is not for us */
        puddle_stream_state others[] = { PUDDLE_STREAM_ACTIVE, PUDDLE_STREAM_CLOSED, PUDDLE_STREAM_PENDING_IN };
        for (int i = 0; i < 3; i++) {
            set_record(&dave, others[i], PUDDLE_OWED_NONE, 1);
            b = peer_body(&dave, dave.uid, "responder", OKPROFILE, i == 1 ? "\"approved\":true" : "\"approved\":false");
            r = signed_post(&dave, "/stream/v1/access/result", b);
            CHECK(r.status == 202 && state_is(&dave, others[i]));
            free(b);
        }
        /* someone we never asked */
        person *strangers[1] = { &dave };
        (void)strangers;
        char path[900];
        snprintf(path, sizeof path, "%s/state/streams/%s.json", dd, dave.uid);
        remove(path);
        puddle_streams_close(ss);
        ss = puddle_streams_open(dd, err, sizeof err);
        if (!ps) ps = puddle_people_open(dd, err, sizeof err);
        ctx.streams = ss; ctx.peer_key_user = ss;
        b = peer_body(&dave, dave.uid, "responder", OKPROFILE, "\"approved\":true");
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && !has(&dave));
        free(b);
    }

    /* what does not authenticate is a plain 404 */
    {
        set_record(&dave, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);
        b = peer_body(&dave, dave.uid, "responder", OKPROFILE, "\"approved\":true");
        CHECK(send_raw("/stream/v1/access/result", b, NULL).status == 404);                      /* unsigned */
        CHECK(signed_post(&carol, "/stream/v1/access/result", b).status == 404);                 /* signed by someone else */
        free(b);
        b = peer_body(&carol, dave.uid, "responder", OKPROFILE, "\"approved\":true");          /* another's chain under Dave's id */
        CHECK(signed_post(&dave, "/stream/v1/access/result", b).status == 404);
        CHECK(signed_post(&carol, "/stream/v1/access/result", b).status == 404);
        free(b);
        b = peer_body(&dave, dave.uid, "responder", "{}", "\"approved\":true");                 /* no display name: still an answer, from someone shown by their id */
        CHECK(signed_post(&dave, "/stream/v1/access/result", b).status == 202 && state_is(&dave, PUDDLE_STREAM_ACTIVE) && !strcmp(their_profile(dave.uid), "{}"));
        free(b);
        set_record(&dave, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);
        b = peer_body(&dave, dave.uid, "requester", OKPROFILE, "\"approved\":true");            /* wrong key for the object */
        CHECK(signed_post(&dave, "/stream/v1/access/result", b).status == 404);
        free(b);
        CHECK(state_is(&dave, PUDDLE_STREAM_PENDING_OUT));
        b = peer_body(&dave, dave.uid, "responder", OKPROFILE, "\"approved\":true");
        char sig[800];
        puddle_reqsig_sign(dave.id, "POST", "/stream/v1/access/result", b, strlen(b), bob.uid, clock_now, sig, sizeof sig);
        CHECK(send_raw("/stream/v1/access/result", b, sig).status == 202);
        CHECK(send_raw("/stream/v1/access/result", b, sig).status == 401);                       /* the same request again */
        free(b);
    }

    /* ---- "is my request still waiting?" ---- */
    {
        person erin; /* someone never heard of */
        make(&erin, "erin");
        const char *st;
#define STATUS(who) (r = signed_post(&(who), "/stream/v1/access/status", (b = peer_body(&(who), (who).uid, "requester", OKPROFILE, NULL))), free(b), \
                     r.status == 200 ? (strstr(body_of(&r), "\"approved\"") ? "approved" : strstr(body_of(&r), "\"declined\"") ? "declined" : strstr(body_of(&r), "\"pending\"") ? "pending" : "?") : "error")
        st = STATUS(erin);
        CHECK(!strcmp(st, "pending"));
        set_record(&erin, PUDDLE_STREAM_PENDING_IN, PUDDLE_OWED_NONE, 1);
        CHECK(!strcmp(STATUS(erin), "pending"));
        set_record(&erin, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, 1);
        st = STATUS(erin);
        CHECK(!strcmp(st, "approved"));
        CHECK(strstr(body_of(&r), "\"responder\"") && strstr(body_of(&r), bob.uid));            /* and who answers */
        set_record(&erin, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_NONE, 1);
        CHECK(!strcmp(STATUS(erin), "declined"));
        char e2[200];
        CHECK(puddle_streams_block(ss, erin.uid, T0, e2, sizeof e2) == 0);                       /* closed AND blocked: they must not be told */
        CHECK(!strcmp(STATUS(erin), "pending"));
        CHECK(puddle_streams_unblock(ss, erin.uid, e2, sizeof e2) == 0);
        set_record(&erin, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, 1);
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_CLOSED, e2, sizeof e2) == 0);
        CHECK(!strcmp(STATUS(erin), "approved"));                                                 /* an existing stream is not "a stranger" */
        CHECK(puddle_streams_set_access(ss, PUDDLE_ACCESS_MANUAL, e2, sizeof e2) == 0);
        /* the question is only answered to whoever signed it */
        b = peer_body(&erin, erin.uid, "requester", OKPROFILE, NULL);
        CHECK(signed_post(&carol, "/stream/v1/access/status", b).status == 404);
        free(b);
        CHECK(send_raw("/stream/v1/access/status", "{}", NULL).status == 404);
        puddle_identity_close(erin.id);
    }

    /* ---- "this stream is closed" ---- */
    {
        set_record(&alice, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_CLOSED, 1);
        clock_now += 1000;
        r = signed_post(&alice, "/stream/v1/close", "{}");
        CHECK(r.status == 204 && !strstr(r.out, "Content-Length") && state_is(&alice, PUDDLE_STREAM_CLOSED));
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && rec.owed == PUDDLE_OWED_NONE && rec.updated_ms == T0 + 1000);   /* nothing more to tell them */
        puddle_stream_free(&rec);
        clock_now += 1000;
        r = signed_post(&alice, "/stream/v1/close", "{}");
        CHECK(r.status == 403 && strstr(body_of(&r), "no_stream"));                              /* already closed */
        set_record(&alice, PUDDLE_STREAM_PENDING_IN, PUDDLE_OWED_NONE, 1);                      /* they withdraw a request they made */
        clock_now += 1000;
        CHECK(signed_post(&alice, "/stream/v1/close", "{}").status == 204 && state_is(&alice, PUDDLE_STREAM_CLOSED));
        set_record(&alice, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, 0);                     /* no keys of theirs yet: no way to check */
        clock_now += 1000;
        CHECK(signed_post(&alice, "/stream/v1/close", "{}").status == 404 && state_is(&alice, PUDDLE_STREAM_PENDING_OUT));
        set_record(&alice, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, 1);
        char e2[200];
        CHECK(puddle_streams_block(ss, alice.uid, T0, e2, sizeof e2) == 0);
        clock_now += 1000;
        CHECK(signed_post(&alice, "/stream/v1/close", "{}").status == 404);                      /* blocked: as if not there */
        CHECK(puddle_streams_unblock(ss, alice.uid, e2, sizeof e2) == 0);
        clock_now += 1000;
        CHECK(signed_post(&carol, "/stream/v1/close", "{}").status == 404);                      /* declined earlier: we hold none of their keys */
    }

    puddle_people_close(ps);
    puddle_streams_close(ss);
    puddle_replay_free(ctx.replay);
    puddle_identity_close(bob.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id); puddle_identity_close(dave.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
