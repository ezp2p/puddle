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
static person bob, alice, carol, dave, erin;
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

static reply signed_for(person *who, const char *path, const char *body, const char *recipient)
{
    char sig[800];
    if (puddle_reqsig_sign(who->id, "POST", path, body, strlen(body), recipient, clock_now, sig, sizeof sig) != 0) exit(2);
    return send_raw(path, body, sig);
}
static reply signed_post(person *who, const char *path, const char *body) { return signed_for(who, path, body, bob.uid); }

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
#define NOTE "\"message\":\"hi\""

/* a request we sent to an address, as flow.c files it */
static void ask_at(const char *address, char ticket[33], char pid[PUDDLE_USER_ID_LEN + 1], puddle_stream_state state)
{
    puddle_stream rec;
    char err[300];
    CHECK(puddle_ticket_new(ticket) == 0 && puddle_provisional_id(ticket, pid) == 0);
    CHECK(puddle_stream_init(&rec, pid, state, T0) == 0);
    rec.provisional = 1;
    snprintf(rec.ticket, sizeof rec.ticket, "%s", ticket);
    snprintf(rec.note, sizeof rec.note, "hello there");
    CHECK(puddle_stream_add_address(&rec, address) == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

static size_t count_all(void)
{
    puddle_stream *l;
    size_t n;
    CHECK(puddle_streams_list(ss, &l, &n) == 0);
    puddle_stream_list_free(l, n);
    return n;
}

static char *with_ticket(person *who, const char *key, const char *verdict, const char *ticket)
{
    char extra[200];
    snprintf(extra, sizeof extra, "%s%s\"ticket\":\"%s\"", verdict ? verdict : "", verdict ? "," : "", ticket);
    return peer_body(who, who->uid, key, OKPROFILE, extra);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-open-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&bob, "bob"); make(&alice, "alice"); make(&carol, "carol"); make(&dave, "dave"); make(&erin, "erin");
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
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .profiles = profiles, .identity = bob.id, .streams = ss, .people = ps, .replay = puddle_replay_new(4096), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply r;
    puddle_stream rec;
    char *b;
    char ticket[33], pid[PUDDLE_USER_ID_LEN + 1];

    /* ---- the made-up id and the ticket ---- */
    {
        char t2[33], p2[PUDDLE_USER_ID_LEN + 1];
        CHECK(puddle_ticket_new(ticket) == 0 && strlen(ticket) == 32 && puddle_ticket_new(t2) == 0 && strcmp(ticket, t2) != 0);
        CHECK(puddle_provisional_id(ticket, pid) == 0 && puddle_id_valid(pid) && puddle_provisional_id(ticket, p2) == 0 && !strcmp(pid, p2));
        CHECK(puddle_provisional_id(t2, p2) == 0 && strcmp(pid, p2) != 0);
        CHECK(puddle_provisional_id("short", p2) != 0 && puddle_provisional_id(NULL, p2) != 0);
    }

    /* ---- a first request names no recipient, and only the access route takes one ---- */
    {
        char *req = peer_body(&alice, alice.uid, "requester", OKPROFILE, NOTE ",\"ticket\":\"0123456789abcdef0123456789abcdef\"");
        r = signed_for(&alice, "/stream/v1/access", req, PUDDLE_RECIPIENT_ANY);
        CHECK(r.status == 202 && state_is(&alice, PUDDLE_STREAM_PENDING_IN));
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && !strcmp(rec.ticket, "0123456789abcdef0123456789abcdef") && !rec.provisional && !strcmp(rec.note, "hi"));
        puddle_stream_free(&rec);
        r = signed_for(&alice, "/stream/v1/access/status", req, PUDDLE_RECIPIENT_ANY);               /* not the other chain-proven route */
        CHECK(r.status == 404);
        r = signed_for(&alice, "/stream/v1/access/result", req, PUDDLE_RECIPIENT_ANY);
        CHECK(r.status == 404);
        r = signed_for(&alice, "/stream/v1/close", "{}", PUDDLE_RECIPIENT_ANY);                        /* nor a route for people we know */
        CHECK(r.status == 404);
        r = signed_for(&alice, "/stream/v1/access", req, carol.uid);                                    /* signed for someone else: as ever */
        CHECK(r.status == 404);
        free(req);
        /* a ticket that is not one is simply not kept */
        req = peer_body(&carol, carol.uid, "requester", OKPROFILE, NOTE ",\"ticket\":\"not-a-ticket\"");
        r = signed_for(&carol, "/stream/v1/access", req, PUDDLE_RECIPIENT_ANY);
        CHECK(r.status == 202 && puddle_streams_get(ss, carol.uid, &rec) == 0 && rec.ticket[0] == '\0');
        puddle_stream_free(&rec);
        free(req);
        /* asking us, from our own key: that is ourselves */
        size_t before = count_all();
        req = peer_body(&bob, bob.uid, "requester", OKPROFILE, NOTE);
        r = signed_for(&bob, "/stream/v1/access", req, PUDDLE_RECIPIENT_ANY);
        CHECK(r.status == 202 && count_all() == before && !has(&bob));
        free(req);
    }

    /* ---- the answer says who lives there ---- */
    {
        ask_at("198.51.100.5:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        size_t before = count_all();
        b = with_ticket(&dave, "responder", "\"approved\":true", ticket);
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&dave, PUDDLE_STREAM_ACTIVE));
        CHECK(puddle_streams_get(ss, pid, &rec) == 1);                                              /* the made-up id is gone */
        CHECK(count_all() == before);                                                               /* refiled, not added */
        CHECK(puddle_streams_get(ss, dave.uid, &rec) == 0 && !rec.provisional && rec.ticket[0] == '\0' && !strcmp(rec.note, "hello there") &&
              rec.chain && strstr(their_profile(dave.uid), "Them") && !strcmp(rec.addresses[0], "198.51.100.5:8443"));
        puddle_stream_free(&rec);
        /* the ticket is spent: someone else answering with it gets nothing */
        b = with_ticket(&erin, "responder", "\"approved\":true", ticket);
        r = signed_post(&erin, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && !has(&erin));
        free(b);
    }
    {
        /* an impostor answering first is who we get (we asked an address, not a person); a second answer is refused */
        ask_at("198.51.100.6:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&erin, "responder", "\"approved\":true", ticket);
        r = signed_post(&erin, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&erin, PUDDLE_STREAM_ACTIVE));
        free(b);
    }
    {
        /* a ticket we never made, a ticket without an answer, and an answer with no ticket from someone unknown */
        size_t before = count_all();
        ask_at("198.51.100.7:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&alice, "responder", "\"approved\":true", "00000000000000000000000000000000");
        r = signed_post(&alice, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&alice, PUDDLE_STREAM_PENDING_IN));                       /* untouched: she is still the one who asked us */
        free(b);
        b = with_ticket(&carol, "responder", "\"approved\":true", "garbage");
        r = signed_post(&carol, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&carol, PUDDLE_STREAM_PENDING_IN));
        free(b);
        puddle_stream st;
        CHECK(puddle_streams_get(ss, pid, &st) == 0 && st.provisional && st.state == PUDDLE_STREAM_PENDING_OUT);   /* still waiting */
        puddle_stream_free(&st);
        CHECK(count_all() == before + 1);
        CHECK(puddle_streams_forget(ss, pid) == 0 && puddle_streams_forget(ss, pid) == 1 && count_all() == before);
        CHECK(puddle_streams_forget(ss, alice.uid) == 1 && has(&alice));                            /* only provisional ones can be forgotten */
    }
    {
        /* declined: closed under their id */
        make(&dave, "dave2");
        ask_at("198.51.100.8:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&dave, "responder", "\"approved\":false", ticket);
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&dave, PUDDLE_STREAM_CLOSED) && puddle_streams_get(ss, pid, &rec) == 1);
        free(b);
    }
    {
        /* we withdrew the request (it stays, closed); they said yes anyway: they are told it is closed */
        person fred;
        make(&fred, "fred");
        ask_at("198.51.100.9:8443", ticket, pid, PUDDLE_STREAM_CLOSED);
        b = with_ticket(&fred, "responder", "\"approved\":true", ticket);
        r = signed_post(&fred, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&fred, PUDDLE_STREAM_CLOSED));
        CHECK(puddle_streams_get(ss, fred.uid, &rec) == 0 && rec.owed == PUDDLE_OWED_CLOSED);
        puddle_stream_free(&rec);
        free(b);
    }
    {
        /* we know them already: a closed stream starts afresh; anything else stands */
        set_record(&dave, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_NONE, 1);
        ask_at("198.51.100.8:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&dave, "responder", "\"approved\":true", ticket);
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&dave, PUDDLE_STREAM_ACTIVE) && puddle_streams_get(ss, pid, &rec) == 1);
        CHECK(puddle_streams_get(ss, dave.uid, &rec) == 0 && !rec.error[0] && !strcmp(rec.note, "hello there"));
        puddle_stream_free(&rec);
        free(b);
        set_record(&dave, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, 1);
        ask_at("198.51.100.8:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&dave, "responder", "\"approved\":false", ticket);
        r = signed_post(&dave, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && state_is(&dave, PUDDLE_STREAM_ACTIVE) && puddle_streams_get(ss, pid, &rec) == 1); /* not closed by a stray no */
        free(b);
    }
    {
        /* blocked: the answer says whose it is, and that is the end of it */
        person gus;
        make(&gus, "gus");
        char e2[200];
        CHECK(puddle_streams_block(ss, gus.uid, T0, e2, sizeof e2) == 0);
        ask_at("198.51.100.10:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        b = with_ticket(&gus, "responder", "\"approved\":true", ticket);
        r = signed_post(&gus, "/stream/v1/access/result", b);
        CHECK(r.status == 202 && !has(&gus) && puddle_streams_get(ss, pid, &rec) == 1);
        free(b);
    }
    {
        /* both asked: we asked an address, and then the person there asks us. Their request tells us who they are. */
        person hal;
        make(&hal, "hal");
        ask_at("198.51.100.5:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);          /* peer_body's address for whoever asks */
        char *req = peer_body(&hal, hal.uid, "requester", OKPROFILE, NOTE);
        r = signed_for(&hal, "/stream/v1/access", req, PUDDLE_RECIPIENT_ANY);
        CHECK(r.status == 202 && state_is(&hal, PUDDLE_STREAM_ACTIVE) && puddle_streams_get(ss, pid, &rec) == 1);
        CHECK(puddle_streams_get(ss, hal.uid, &rec) == 0 && rec.owed == PUDDLE_OWED_APPROVED);
        puddle_stream_free(&rec);
        free(req);
    }

    /* ---- the provisional record survives a restart, and is not a peer ---- */
    {
        ask_at("198.51.100.11:8443", ticket, pid, PUDDLE_STREAM_PENDING_OUT);
        puddle_people_close(ps);
    puddle_streams_close(ss);
        ss = puddle_streams_open(dd, err, sizeof err);
        if (!ps) ps = puddle_people_open(dd, err, sizeof err);
        ctx.streams = ss; ctx.peer_key_user = ss;
        CHECK(puddle_streams_find_ticket(ss, ticket, &rec) == 0 && !strcmp(rec.id, pid) && rec.provisional);
        puddle_stream_free(&rec);
        CHECK(puddle_streams_find_provisional(ss, "198.51.100.11:8443", &rec) == 0 && !strcmp(rec.id, pid));
        puddle_stream_free(&rec);
        CHECK(puddle_streams_find_ticket(ss, "", &rec) == 1 && puddle_streams_find_ticket(ss, "00000000000000000000000000000000", &rec) == 1);
        unsigned char pub[32];
        CHECK(puddle_streams_peer_key(ss, pid, "0000000000000000000000000000000000000000000000000000000000000000", pub) == 0); /* no key, so no voice */
        CHECK(puddle_streams_adopt(ss, alice.uid, carol.uid, err, sizeof err) == -1);                /* not provisional */
        CHECK(puddle_streams_adopt(ss, pid, pid, err, sizeof err) == -1);
    }

    char cmd[900];
    snprintf(cmd, sizeof cmd, "%s", root);
    nftw(cmd, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
