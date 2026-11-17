#include "federation/groupapi.h"
#include "core/people.h"
#include "federation/groupflow.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "events/message.h"
#include "federation/messages.h"
#include "core/phrase.h"
#include "service/routes.h"
#include "federation/courier.h"
#include "federation/sender.h"
#include "http/client.h"
#include "events/group_events.h"

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
static person me, alice, bob, carol, dave, erin, ivy, jon;
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
static puddle_groups *gs;
static puddle_store *store;
static puddle_outbox *ob;
static puddle_config cfg;

static void make(person *p, const char *name)
{
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!p->id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

static void set_peer(person *p, puddle_stream_state state)
{
    puddle_stream rec;
    char err[300];
    size_t cl;
    char *chain = puddle_identity_chain(p->id, &cl);
    CHECK(puddle_stream_init(&rec, p->uid, state, T0) == 0);
    CHECK(puddle_stream_set_chain(&rec, chain, cl) == 0);
    free(chain);
    CHECK(puddle_stream_add_address(&rec, "203.0.113.9:8443") == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

typedef struct { int status; char out[40000]; size_t n; } reply;

static reply *call(const char *path, const char *body, const char *sig)
{
    static reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "POST");
    snprintf(req.target, sizeof req.target, "%s", path);
    snprintf(req.path, sizeof req.path, "%s", path);
    if (sig) snprintf(req.signature, sizeof req.signature, "%s", sig);
    size_t bl = strlen(body);
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    memcpy(copy, body, bl + 1);
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, 0, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    return &rp;
}
static const char *body_of(const reply *r) { const char *b = strstr(r->out, "\r\n\r\n"); return b ? b + 4 : ""; }

static reply *post_as(person *from, const char *path, const char *body)
{
    char sig[800];
    if (puddle_reqsig_sign(from->id, "POST", path, body, strlen(body), me.uid, clock_now, sig, sizeof sig) != 0) exit(2);
    return call(path, body, sig);
}

/* ---- events as another person made them ---- */

typedef struct { char id[65]; char sig[PUDDLE_SIG_TEXT_MAX]; char *text; } env;

static env sign_env(person *who, const char *event, size_t el)
{
    env e;
    e.text = strdup(event);
    puddle_sha256_hex(event, el, e.id);
    puddle_event_sign(who->id, event, el, e.sig);
    return e;
}

static char *delivery_json(const char *chat, env *es, size_t n)
{
    char *buf = malloc(1 << 18);
    puddle_jw w;
    puddle_jw_init(&w, buf, 1 << 18);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_key(&w, "events");
    puddle_jw_array(&w);
    for (size_t i = 0; i < n; i++) {
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", es[i].id);
        puddle_jw_str(&w, "event", es[i].text);
        puddle_jw_str(&w, "sig", es[i].sig);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    return buf;
}

/* `from` delivers one event to the chat; the answer */
static reply *deliver_one(person *from, const char *chat, person *signer, const char *event)
{
    env e = sign_env(signer, event, strlen(event));
    char *b = delivery_json(chat, &e, 1);
    reply *r = post_as(from, "/stream/v1/events", b);
    free(b);
    free(e.text);
    return r;
}

static int has_text(const reply *r, const char *needle) { return strstr(body_of(r), needle) != NULL; }

/* ---- what is queued ---- */

static int queued(const char *peer, const char *kind)
{
    puddle_ob_item *l;
    size_t n;
    int c = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        c += !strcmp(l[i].peer, peer) && (!kind || !strcmp(l[i].kind, kind)) && l[i].state == PUDDLE_OB_QUEUED;
    puddle_outbox_list_free(l, n);
    return c;
}

static char *last_body(const char *peer, const char *kind)
{
    puddle_ob_item *l;
    size_t n;
    char *b = NULL;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(l[i].peer, peer) && !strcmp(l[i].kind, kind)) {
            free(b);
            b = strdup(l[i].body);
        }
    puddle_outbox_list_free(l, n);
    return b;
}

static size_t stored(const char *chat)
{
    puddle_event_list l;
    char err[200];
    if (puddle_store_tail(store, chat, 10000, &l, err, sizeof err) != 0) return (size_t)-1;
    size_t n = l.count;
    puddle_event_list_free(&l);
    return n;
}

static int in_state(const char *gid, const char *who, puddle_gm_state want)
{
    puddle_group g;
    if (puddle_groups_get(gs, gid, &g) != 0) return 0;
    puddle_gmember *m = puddle_group_member(&g, who);
    int ok = m && m->state == want;
    puddle_group_free(&g);
    return ok;
}
static int in_state_stream(const char *peer, puddle_stream_state want)
{
    puddle_stream st;
    if (puddle_streams_get(ss, peer, &st) != 0) return 0;
    int ok = st.state == want;
    puddle_stream_free(&st);
    return ok;
}
static int my_state(const char *gid, puddle_group_my want)
{
    puddle_group g;
    if (puddle_groups_get(gs, gid, &g) != 0) return 0;
    int ok = g.my == want;
    puddle_group_free(&g);
    return ok;
}

/* {"<key>":{id,address,profile,chain}<tail>} for `who` (the chain is JSON text and has to be escaped, so these are built with the writer) */
static char *who_json(person *who, const char *key, const char *address, const char *name, const char *tail)
{
    size_t cl;
    char *chain = puddle_identity_chain(who->id, &cl);
    char *b = malloc(1 << 16), prof[200];
    snprintf(prof, sizeof prof, "{\"display_name\":\"%s\"}", name);
    puddle_jw w;
    puddle_jw_init(&w, b, 1 << 16);
    puddle_jw_object(&w);
    puddle_jw_key(&w, key);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", who->uid);
    puddle_jw_str(&w, "address", address);
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, prof, strlen(prof));
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    free(chain);
    if (!n) exit(2);
    if (tail && *tail) { b[n - 1] = ','; strcpy(b + n, tail); strcat(b, "}"); }
    return b;
}

/* `who` asks for a group-only connection, with `join_ev` (signed by `signer`) as the proof; recipient `as_whom` */
static reply *scoped_post(person *who, const char *gid, const char *join_ev, person *signer, const char *address, const char *as_whom)
{
    size_t cl;
    char *chain = puddle_identity_chain(who->id, &cl);
    env j = sign_env(signer, join_ev, strlen(join_ev));
    char *b = malloc(1 << 17);
    puddle_jw w;
    puddle_jw_init(&w, b, 1 << 17);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "requester");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", who->uid);
    puddle_jw_str(&w, "address", address);
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, "{\"display_name\":\"Someone\"}", 26);
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    puddle_jw_key(&w, "scope");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "group", gid);
    puddle_jw_key(&w, "join");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "event", join_ev);
    puddle_jw_str(&w, "sig", j.sig);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    char sig[800];
    if (puddle_reqsig_sign(who->id, "POST", "/stream/v1/access", b, strlen(b), as_whom, clock_now, sig, sizeof sig) != 0) exit(2);
    reply *r = call("/stream/v1/access", b, sig);
    free(b);
    free(chain);
    free(j.text);
    return r;
}

/* an invitation to us from `inviter`, who made the group `creator` made (the creator may be the inviter) */
static person *g_invite_signer; /* when set, the invitation is signed by this person instead of its sender */
static char *package(person *inviter, person *creator, const char *gid, const char *name, const char *nonce, int64_t create_ts, int64_t invite_ts,
                     const char *extra_members, const char *group_override, char *create_out, size_t create_cap)
{
    char cev[1024], iev[512], chat[53];
    size_t cl = puddle_group_create_build(creator->uid, create_ts, nonce, name, chat, cev, sizeof cev);
    size_t il = puddle_group_invite_build(chat, inviter->uid, invite_ts, me.uid, iev, sizeof iev);
    CHECK(cl > 0 && il > 0);
    if (gid) CHECK(!strcmp(gid, chat));
    env c = sign_env(creator, cev, cl), i = sign_env(g_invite_signer ? g_invite_signer : inviter, iev, il);
    char *b = malloc(1 << 16);
    puddle_jw w;
    puddle_jw_init(&w, b, 1 << 16);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "group", group_override ? group_override : chat);
    puddle_jw_key(&w, "create");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "event", cev);
    puddle_jw_str(&w, "sig", c.sig);
    puddle_jw_end_object(&w);
    puddle_jw_key(&w, "invite");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "event", iev);
    puddle_jw_str(&w, "sig", i.sig);
    puddle_jw_end_object(&w);
    puddle_jw_key(&w, "members");
    puddle_jw_raw(&w, extra_members, strlen(extra_members));
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    if (create_out) snprintf(create_out, create_cap, "%s", chat);
    free(c.text); free(i.text);
    return b;
}

/* ,{"id","state","ts"} for the roster's extra members, built with a person's real id */
static char *extra_member(person *p, const char *state, int64_t ts)
{
    static char buf[4][300];
    static int k;
    char *b = buf[k++ % 4];
    snprintf(b, 300, "{\"id\":\"%s\",\"state\":\"%s\",\"ts\":%lld}", p->uid, state, (long long)ts);
    return b;
}

static char *roster(person *a_member, person *invited_me, const char *extra)
{
    static char r[2000];
    snprintf(r, sizeof r, "[{\"id\":\"%s\",\"state\":\"member\",\"ts\":%lld,\"address\":\"203.0.113.50:8443\"},{\"id\":\"%s\",\"state\":\"invited\",\"ts\":%lld}%s%s]",
             a_member->uid, (long long)T0, invited_me->uid, (long long)T0 + 10, extra && *extra ? "," : "", extra ? extra : "");
    return r;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-groupapi-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&bob, "bob"); make(&carol, "carol"); make(&dave, "dave"); make(&erin, "erin"); make(&ivy, "ivy"); make(&jon, "jon");
    char err[300], data[700];
    snprintf(data, sizeof data, "%s/data", root);
    store = puddle_store_open(data, err, sizeof err);
    ss = puddle_streams_open(data, err, sizeof err);
    if (!ps) ps = puddle_people_open(data, err, sizeof err);
    gs = puddle_groups_open(data, err, sizeof err);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .identity = me.id, .store = store, .streams = ss, .people = ps, .groups = gs, .outbox = ob, .replay = puddle_replay_new(4096), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    set_peer(&alice, PUDDLE_STREAM_ACTIVE);
    set_peer(&bob, PUDDLE_STREAM_ACTIVE);
    set_peer(&carol, PUDDLE_STREAM_ACTIVE);
    set_peer(&dave, PUDDLE_STREAM_SCOPED);
    const char *code;
    char gid[53], id[65], event[2000];

    /* ================= the owner's side ================= */

    /* ---- make a group ---- */
    CHECK(puddle_group_create(&ctx, "", gid, &code) == 400 && !strcmp(code, "bad_name"));
    CHECK(puddle_group_create(&ctx, "two\nlines", gid, &code) == 400);
    CHECK(puddle_group_create(&ctx, "Plans", gid, &code) == 0 && puddle_id_valid(gid));
    CHECK(my_state(gid, PUDDLE_GROUP_IN) && in_state(gid, me.uid, PUDDLE_GM_MEMBER) && stored(gid) == 1);
    {
        puddle_group g;
        CHECK(puddle_groups_get(gs, gid, &g) == 0 && !strcmp(g.name, "Plans") && !strcmp(g.creator, me.uid) && g.create_event[0] && g.create_sig[0]);
        puddle_gevent ge;
        const char *why;
        CHECK(puddle_group_event_parse(g.create_event, strlen(g.create_event), &ge, &why) == 0 && !strcmp(ge.chat, gid));
        puddle_group_free(&g);
    }
    {
        puddle_ctx nogroups = ctx;
        nogroups.groups = NULL;
        CHECK(puddle_group_create(&nogroups, "x", gid, &code) == 500);
    }
    char g1[53];
    memcpy(g1, gid, sizeof g1);

    /* ---- invite ---- */
    CHECK(puddle_group_invite(&ctx, g1, alice.uid, &code) == 0);
    CHECK(in_state(g1, alice.uid, PUDDLE_GM_INVITED) && queued(alice.uid, "group_invite") == 1 && queued(alice.uid, "events") == 0);
    {
        char *b = last_body(alice.uid, "group_invite");
        CHECK(b && strstr(b, g1) && strstr(b, "\"create\":{\"event\":\"") && strstr(b, "\"invite\":{\"event\":\"") && strstr(b, me.uid) && strstr(b, "\"address\":\"127.0.0.1:8443\""));
        free(b);
    }
    CHECK(puddle_group_invite(&ctx, g1, alice.uid, &code) == 409 && !strcmp(code, "already_in"));
    CHECK(puddle_group_invite(&ctx, g1, me.uid, &code) == 400);
    CHECK(puddle_group_invite(&ctx, g1, dave.uid, &code) == 403 && !strcmp(code, "no_stream"));     /* not a friend: a scoped stream is not enough */
    CHECK(puddle_group_invite(&ctx, "daaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", alice.uid, &code) == 404);
    CHECK(puddle_group_invite(&ctx, g1, "nope", &code) == 400);
    CHECK(puddle_group_invite(&ctx, g1, carol.uid, &code) == 0 && queued(carol.uid, "group_invite") == 1);
    CHECK(queued(alice.uid, "events") == 1);                                                    /* alice, invited, hears who else was invited; she is not a member yet */

    /* alice says yes: her join arrives (she is a friend, so it comes as an event) */
    {
        char gen[300];
        clock_now += 1000;
        CHECK(puddle_group_join_build(g1, alice.uid, clock_now, gen, sizeof gen) > 0);
        reply *r = deliver_one(&alice, g1, &alice, gen);
        CHECK(r->status == 200 && has_text(r, "\"accepted\":[\""));
        CHECK(in_state(g1, alice.uid, PUDDLE_GM_MEMBER));
    }
    /* now the others hear what happens */
    CHECK(puddle_group_invite(&ctx, g1, bob.uid, &code) == 0);
    CHECK(queued(alice.uid, "events") == 2 && queued(carol.uid, "events") == 1);                /* the member is told of the invitation, and so is the other invited */
    CHECK(queued(bob.uid, "events") == 0);                                                      /* the one invited is told by the invitation itself */
    {
        char *b = last_body(bob.uid, "group_invite");
        CHECK(b && strstr(b, alice.uid) && strstr(b, "203.0.113.9:8443"));                      /* alice, as we reach her */
        free(b);
    }
    CHECK(puddle_group_rename(&ctx, g1, "Plans 2", &code) == 0 && queued(alice.uid, "events") == 3 && queued(bob.uid, "events") == 1 && queued(carol.uid, "events") == 2);
    CHECK(puddle_group_rename(&ctx, g1, "", &code) == 400 && !strcmp(code, "bad_name"));
    {
        puddle_group g;
        CHECK(puddle_groups_get(gs, g1, &g) == 0 && !strcmp(g.name, "Plans 2"));
        puddle_group_free(&g);
    }
    /* a message goes to those who are members and reachable */
    {
        int sent = -1;
        clock_now += 1000;
        size_t el = puddle_message_build(g1, me.uid, puddle_event_next_ts(clock_now), "hello all", 9, NULL, event, sizeof event);
        CHECK(el > 0 && puddle_group_send_event(&ctx, g1, event, el, id, &sent, &code) == 0 && sent == 1);
        CHECK(queued(alice.uid, "events") == 4 && queued(bob.uid, "events") == 1 && queued(carol.uid, "events") == 2); /* a message goes to members only */
        CHECK(stored(g1) >= 5);
        set_peer(&alice, PUDDLE_STREAM_CLOSED);                                                 /* nobody can be reached */
        el = puddle_message_build(g1, me.uid, puddle_event_next_ts(clock_now), "anyone?", 7, NULL, event, sizeof event);
        CHECK(puddle_group_send_event(&ctx, g1, event, el, id, &sent, &code) == 0 && sent == 0 && queued(alice.uid, "events") == 4);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE);
        puddle_ctx noout = ctx;
        noout.outbox = NULL;
        el = puddle_message_build(g1, me.uid, puddle_event_next_ts(clock_now), "lost", 4, NULL, event, sizeof event);
        CHECK(puddle_group_send_event(&noout, g1, event, el, id, &sent, &code) == 500 && !strcmp(code, "not_queued"));  /* kept, but could not be queued */
        CHECK(puddle_group_send_event(&ctx, "daaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", event, el, id, &sent, &code) == 404);
    }

    /* ---- leaving ---- */
    {
        int before = queued(alice.uid, "events");
        CHECK(puddle_group_leave(&ctx, g1, &code) == 0 && my_state(g1, PUDDLE_GROUP_LEFT) && in_state(g1, me.uid, PUDDLE_GM_LEFT));
        CHECK(queued(alice.uid, "events") == before + 1);
        CHECK(puddle_group_leave(&ctx, g1, &code) == 409 && !strcmp(code, "not_in_group"));
        CHECK(puddle_group_rename(&ctx, g1, "Mine", &code) == 409);
        CHECK(puddle_group_invite(&ctx, g1, dave.uid, &code) == 409);
        int sent;
        size_t el = puddle_message_build(g1, me.uid, puddle_event_next_ts(clock_now), "hi", 2, NULL, event, sizeof event);
        CHECK(puddle_group_send_event(&ctx, g1, event, el, id, &sent, &code) == 409);
        CHECK(puddle_group_accept(&ctx, g1, &code) == 409 && !strcmp(code, "not_invited"));
        CHECK(puddle_group_decline(&ctx, g1, &code) == 409);
    }

    /* ================= being invited ================= */

    char g2[53];
    clock_now += 10000;
    {
        char *pk = package(&alice, &alice, NULL, "Alice's group", "0123456789abcdef0123456789abcdef", T0 + 50, clock_now, roster(&alice, &me, ""), NULL, g2, sizeof g2);
        reply *r = post_as(&alice, "/stream/v1/group/invite", pk);
        CHECK(r->status == 202);
        CHECK(my_state(g2, PUDDLE_GROUP_INVITED) && stored(g2) == 2);
        puddle_group g;
        CHECK(puddle_groups_get(gs, g2, &g) == 0 && !strcmp(g.name, "Alice's group") && !strcmp(g.invited_by, alice.uid) && !strcmp(g.creator, alice.uid) &&
              puddle_group_member(&g, alice.uid)->state == PUDDLE_GM_MEMBER && !strcmp(puddle_group_member(&g, alice.uid)->address, "203.0.113.50:8443") &&
              puddle_group_member(&g, me.uid)->state == PUDDLE_GM_INVITED && puddle_group_member(&g, me.uid)->ts == clock_now && g.create_event[0]);
        puddle_group_free(&g);
        CHECK(puddle_groups_pending(gs) == 1);
        r = post_as(&alice, "/stream/v1/group/invite", pk);                                       /* again: nothing new */
        CHECK(r->status == 202 && stored(g2) == 2 && puddle_groups_pending(gs) == 1);
        free(pk);
    }
    {
        /* what is refused */
        char tmp[53];
        struct { const char *what; char *b; int status; } cases[12];
        int n = 0;
        cases[n].what = "a stranger"; cases[n].b = package(&dave, &dave, NULL, "G", "11111111111111111111111111111111", T0 + 60, clock_now, roster(&dave, &me, ""), NULL, tmp, sizeof tmp); cases[n++].status = 403; /* dave is no friend */
        cases[n].what = "wrong id"; cases[n].b = package(&bob, &bob, NULL, "G", "22222222222222222222222222222222", T0 + 60, clock_now, roster(&bob, &me, ""), "caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", tmp, sizeof tmp); cases[n++].status = 400;
        cases[n].what = "sender not in roster"; cases[n].b = package(&bob, &bob, NULL, "G", "33333333333333333333333333333333", T0 + 60, clock_now, roster(&carol, &me, ""), NULL, tmp, sizeof tmp); cases[n++].status = 400;
        cases[n].what = "no one invited (us)"; cases[n].b = package(&bob, &bob, NULL, "G", "44444444444444444444444444444444", T0 + 60, clock_now, "[{\"id\":\"baaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"state\":\"member\",\"ts\":1760000000000}]", NULL, tmp, sizeof tmp); cases[n++].status = 400;
        cases[n].what = "a duplicate in the roster"; cases[n].b = package(&bob, &bob, NULL, "G", "55555555555555555555555555555555", T0 + 60, clock_now, roster(&bob, &me, extra_member(&bob, "member", 1)), NULL, tmp, sizeof tmp); cases[n++].status = 400;
        cases[n].what = "a bad state"; cases[n].b = package(&bob, &bob, NULL, "G", "66666666666666666666666666666666", T0 + 60, clock_now, roster(&bob, &me, extra_member(&carol, "king", 1)), NULL, tmp, sizeof tmp); cases[n++].status = 400;
        cases[n].what = "sender not in the roster (the creator is)"; cases[n].b = package(&bob, &carol, NULL, "G", "12121212121212121212121212121212", T0 + 60, clock_now, roster(&carol, &me, ""), NULL, tmp, sizeof tmp); cases[n++].status = 400;
        g_invite_signer = &carol;
        cases[n].what = "invitation signed by someone else"; cases[n].b = package(&bob, &bob, NULL, "G", "13131313131313131313131313131313", T0 + 60, clock_now, roster(&bob, &me, ""), NULL, tmp, sizeof tmp); cases[n++].status = 400;
        g_invite_signer = NULL;
        for (int i = 0; i < n; i++) {
            reply *r = post_as(i == 0 ? &dave : &bob, "/stream/v1/group/invite", cases[i].b);
            if (r->status != cases[i].status) { fprintf(stderr, "FAIL invitation (%s): %d\n", cases[i].what, r->status); failures++; }
            free(cases[i].b);
        }
        CHECK(puddle_groups_pending(gs) == 1);
        /* the creator is someone else: the invitation is bob's word, not the creator's */
        char *pk = package(&bob, &carol, NULL, "From carol", "77777777777777777777777777777777", T0 + 70, clock_now, roster(&bob, &me, extra_member(&carol, "member", T0 + 1)), NULL, tmp, sizeof tmp);
        reply *r = post_as(&bob, "/stream/v1/group/invite", pk);
        CHECK(r->status == 202 && my_state(tmp, PUDDLE_GROUP_INVITED) && puddle_groups_pending(gs) == 2);
        free(pk);
        /* someone who accepted before we did asks for a group connection while we are still only invited: it is taken, not dropped.
         * (Dropping it left the one who accepted first cut off from everyone who had not yet said yes.) */
        {
            char jev[800];
            puddle_stream st;
            CHECK(puddle_group_join_build(tmp, carol.uid, clock_now, jev, sizeof jev) > 0);
            r = scoped_post(&carol, tmp, jev, &carol, "198.51.100.21:8443", me.uid);
            CHECK(r->status == 202 && my_state(tmp, PUDDLE_GROUP_INVITED));
            CHECK(puddle_streams_get(ss, carol.uid, &st) == 0 && st.group_ok && st.owed == PUDDLE_OWED_SCOPED);
            puddle_stream_free(&st);
            /* and if we then say no, the connection that was only for that group is let go */
            const char *dcode;
            CHECK(puddle_group_decline(&ctx, tmp, &dcode) == 0);
            CHECK(puddle_streams_get(ss, carol.uid, &st) == 0 && !st.group_ok);
            puddle_stream_free(&st);
        }
        /* a tampered invitation: signed by someone who is not the sender */
        pk = package(&bob, &bob, NULL, "Forged", "88888888888888888888888888888888", T0 + 80, clock_now, roster(&bob, &me, ""), NULL, tmp, sizeof tmp);
        r = post_as(&carol, "/stream/v1/group/invite", pk);                                       /* carol sends bob's invitation */
        CHECK(r->status == 400);
        free(pk);
    }

    /* ---- accept ---- */
    {
        int before = queued(alice.uid, "events");
        CHECK(puddle_group_accept(&ctx, g2, &code) == 0 && my_state(g2, PUDDLE_GROUP_IN) && in_state(g2, me.uid, PUDDLE_GM_MEMBER));
        CHECK(queued(alice.uid, "events") == before + 1);                                       /* alice hears the join */
        puddle_group g;
        CHECK(puddle_groups_get(gs, g2, &g) == 0 && g.join_event[0] && g.join_sig[0]);
        puddle_group_free(&g);
        CHECK(puddle_group_accept(&ctx, g2, &code) == 409);
    }
    /* a blocked member stops an acceptance */
    {
        char tmp[53], err2[200];
        char *pk = package(&bob, &bob, NULL, "With carol", "99999999999999999999999999999999", T0 + 90, clock_now,
                           roster(&bob, &me, extra_member(&carol, "member", T0 + 1)), NULL, tmp, sizeof tmp);
        CHECK(post_as(&bob, "/stream/v1/group/invite", pk)->status == 202);
        free(pk);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err2, sizeof err2) == 0);
        CHECK(puddle_group_accept(&ctx, tmp, &code) == 403 && !strcmp(code, "blocked_member") && my_state(tmp, PUDDLE_GROUP_INVITED));
        CHECK(puddle_streams_unblock(ss, carol.uid, err2, sizeof err2) == 0);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        /* decline: only the one who invited us is told */
        int to_bob = queued(bob.uid, "events"), to_alice = queued(alice.uid, "events"), to_carol = queued(carol.uid, "events");
        CHECK(puddle_group_decline(&ctx, tmp, &code) == 0 && my_state(tmp, PUDDLE_GROUP_DECLINED) && in_state(tmp, me.uid, PUDDLE_GM_LEFT));
        CHECK(queued(bob.uid, "events") == to_bob + 1 && queued(alice.uid, "events") == to_alice && queued(carol.uid, "events") == to_carol);
        CHECK(puddle_group_decline(&ctx, tmp, &code) == 409);
        /* the same invitation again is old news; a later one asks again */
        {
            char t3[53];
            int64_t old_ts = clock_now - 100000;
            pk = package(&bob, &bob, NULL, "With carol", "99999999999999999999999999999999", T0 + 90, old_ts,
                         roster(&bob, &me, extra_member(&carol, "member", T0 + 1)), NULL, t3, sizeof t3);
            CHECK(post_as(&bob, "/stream/v1/group/invite", pk)->status == 202 && my_state(tmp, PUDDLE_GROUP_DECLINED));
            free(pk);
            clock_now += 50000;
            pk = package(&bob, &bob, NULL, "With carol", "99999999999999999999999999999999", T0 + 90, clock_now,
                         roster(&bob, &me, extra_member(&carol, "member", T0 + 1)), NULL, t3, sizeof t3);
            CHECK(post_as(&bob, "/stream/v1/group/invite", pk)->status == 202 && my_state(tmp, PUDDLE_GROUP_INVITED));
            free(pk);
            CHECK(puddle_group_decline(&ctx, tmp, &code) == 0);
        }
    }

    /* ================= events from the others ================= */
    {
        char ev[1500];
        size_t n0 = stored(g2);
        /* a member's message */
        clock_now += 1000;
        CHECK(puddle_message_build(g2, alice.uid, clock_now, "hi from alice", 13, NULL, ev, sizeof ev) > 0);
        reply *r = deliver_one(&alice, g2, &alice, ev);
        CHECK(r->status == 200 && has_text(r, "\"accepted\":[\"") && stored(g2) == n0 + 1);
        r = deliver_one(&alice, g2, &alice, ev);                                                 /* the same again */
        CHECK(r->status == 200 && has_text(r, "\"duplicate\":[\"") && stored(g2) == n0 + 1);
        /* a friend who is not in the group */
        CHECK(puddle_message_build(g2, carol.uid, clock_now, "let me in", 9, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&carol, g2, &carol, ev);
        CHECK(r->status == 200 && has_text(r, "not_a_member") && stored(g2) == n0 + 1);
        CHECK(puddle_group_rename_build(g2, carol.uid, clock_now, "Mine", ev, sizeof ev) > 0);
        r = deliver_one(&carol, g2, &carol, ev);
        CHECK(has_text(r, "not_a_member"));
        /* one member speaking for another, or in another chat */
        CHECK(puddle_message_build(g2, bob.uid, clock_now, "as bob", 6, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "wrong_sender"));
        CHECK(puddle_group_rename_build(g2, bob.uid, clock_now, "Bobs", ev, sizeof ev) > 0);                      /* a membership event in someone else's name */
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "wrong_sender"));
        CHECK(puddle_message_build(g1, alice.uid, clock_now, "wrong chat", 10, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "wrong_chat"));
        /* a group is not made by an event */
        {
            char chat[53], cev[1024];
            CHECK(puddle_group_create_build(alice.uid, T0 + 99, "abababababababababababababababab", "New", chat, cev, sizeof cev) > 0);
            r = deliver_one(&alice, chat, &alice, cev);
            CHECK(r->status == 400);                                                              /* no such group here */
            r = deliver_one(&alice, g2, &alice, cev);
            CHECK(has_text(r, "wrong_chat") || has_text(r, "use_invite"));
        }
        /* nothing about this group is known here */
        CHECK(puddle_message_build("eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", alice.uid, clock_now, "x", 1, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&alice, "eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &alice, ev);
        CHECK(r->status == 400);
        /* a bad signature: carol signs what alice "sent" */
        CHECK(puddle_message_build(g2, alice.uid, clock_now + 5, "forged", 6, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &carol, ev);
        CHECK(has_text(r, "unknown_key") || has_text(r, "bad_signature"));
        /* membership: alice invites dave, dave (a group-only peer) says yes, then speaks */
        clock_now += 1000;
        CHECK(puddle_group_invite_build(g2, alice.uid, clock_now, dave.uid, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "\"accepted\":[\"") && in_state(g2, dave.uid, PUDDLE_GM_INVITED));
        CHECK(puddle_message_build(g2, dave.uid, clock_now + 1, "too early", 9, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&dave, g2, &dave, ev);
        CHECK(has_text(r, "not_a_member"));
        clock_now += 2000;
        CHECK(puddle_group_join_build(g2, dave.uid, clock_now, ev, sizeof ev) > 0);
        r = deliver_one(&dave, g2, &dave, ev);
        CHECK(has_text(r, "\"accepted\":[\"") && in_state(g2, dave.uid, PUDDLE_GM_MEMBER));
        CHECK(puddle_message_build(g2, dave.uid, clock_now + 1, "hello from dave", 15, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&dave, g2, &dave, ev);
        CHECK(has_text(r, "\"accepted\":[\""));
        /* a group-only peer reaches the group and nothing else */
        {
            char dm[53], dmev[800];
            puddle_dm_chat_id(me.uid, dave.uid, dm);
            CHECK(puddle_message_build(dm, dave.uid, clock_now + 2, "a private word", 14, NULL, dmev, sizeof dmev) > 0);
            r = deliver_one(&dave, dm, &dave, dmev);
            CHECK(r->status == 403);
        }
        /* the name, from a member */
        CHECK(puddle_group_rename_build(g2, dave.uid, clock_now + 3, "Daves renamed", ev, sizeof ev) > 0);
        r = deliver_one(&dave, g2, &dave, ev);
        {
            puddle_group g;
            CHECK(has_text(r, "\"accepted\":[\"") && puddle_groups_get(gs, g2, &g) == 0 && !strcmp(g.name, "Daves renamed"));
            puddle_group_free(&g);
        }
        /* leaving: no more voice */
        clock_now += 5000;
        CHECK(puddle_group_leave_build(g2, alice.uid, clock_now, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "\"accepted\":[\"") && in_state(g2, alice.uid, PUDDLE_GM_LEFT));
        CHECK(puddle_message_build(g2, alice.uid, clock_now + 1, "still here?", 11, NULL, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "not_a_member"));
        /* old news is not applied again */
        CHECK(puddle_group_join_build(g2, alice.uid, clock_now - 4000, ev, sizeof ev) > 0);
        r = deliver_one(&alice, g2, &alice, ev);
        CHECK(has_text(r, "not_invited") && in_state(g2, alice.uid, PUDDLE_GM_LEFT));
    }
    /* while we are only invited, messages are not taken, but who is in the group still is */
    {
        char tmp[53], ev[800];
        char *pk = package(&bob, &bob, NULL, "Waiting", "abcdefabcdefabcdefabcdefabcdefab", T0 + 120, clock_now, roster(&bob, &me, ""), NULL, tmp, sizeof tmp);
        CHECK(post_as(&bob, "/stream/v1/group/invite", pk)->status == 202);
        free(pk);
        clock_now += 100;
        CHECK(puddle_message_build(tmp, bob.uid, clock_now, "are you coming", 14, NULL, ev, sizeof ev) > 0);
        reply *r = deliver_one(&bob, tmp, &bob, ev);
        CHECK(has_text(r, "not_in_group"));
        CHECK(puddle_group_leave_build(tmp, bob.uid, clock_now + 10, ev, sizeof ev) > 0);
        r = deliver_one(&bob, tmp, &bob, ev);
        CHECK(has_text(r, "\"accepted\":[\"") && in_state(tmp, bob.uid, PUDDLE_GM_LEFT));
    }

    /* ================= group-only connections ================= */

    /* a request from `who` for a group-only connection, with their own member.join as proof; `signer` signs the join (normally who) */
    char g6[53];
    CHECK(puddle_group_create(&ctx, "Meet", g6, &code) == 0);
    CHECK(puddle_group_invite(&ctx, g6, alice.uid, &code) == 0);
    {
        char ev[800];
        clock_now += 2000;
        CHECK(puddle_group_join_build(g6, alice.uid, clock_now, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&alice, g6, &alice, ev), "\"accepted\":[\""));
        clock_now += 2000;
        CHECK(puddle_group_invite_build(g6, alice.uid, clock_now, dave.uid, ev, sizeof ev) > 0);        /* alice brings dave in */
        CHECK(has_text(deliver_one(&alice, g6, &alice, ev), "\"accepted\":[\"") && in_state(g6, dave.uid, PUDDLE_GM_INVITED));
        CHECK(puddle_group_invite_build(g6, alice.uid, clock_now + 1, erin.uid, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&alice, g6, &alice, ev), "\"accepted\":[\"") && in_state(g6, erin.uid, PUDDLE_GM_INVITED));
    }
    {
        char join[800], eid[65];
        (void)eid;
        reply *r;
        /* what is refused, each in turn: the connection is not made, and the answer is the same empty 202 */
        clock_now += 5000;
        CHECK(puddle_group_join_build(g6, dave.uid, clock_now, join, sizeof join) > 0);
        set_peer(&dave, PUDDLE_STREAM_CLOSED);
        {
            puddle_stream st;
            CHECK(puddle_streams_get(ss, dave.uid, &st) == 0);
            puddle_stream_free(&st);
        }
        /* unknown group */
        {
            char other[53], jev[800];
            CHECK(puddle_group_join_build("eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", dave.uid, clock_now, jev, sizeof jev) > 0);
            (void)other;
            r = scoped_post(&dave, "eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", jev, &dave, "198.51.100.20:8443", me.uid);
            CHECK(r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_INVITED));
        }
        /* a join signed by someone else, from someone else, or for someone else */
        r = scoped_post(&dave, g6, join, &carol, "198.51.100.20:8443", me.uid);
        CHECK(r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_INVITED));
        {
            char jev[800];
            CHECK(puddle_group_join_build(g6, carol.uid, clock_now, jev, sizeof jev) > 0);                  /* carol was not invited */
            r = scoped_post(&carol, g6, jev, &carol, "198.51.100.21:8443", me.uid);
            CHECK(r->status == 202 && !in_state(g6, carol.uid, PUDDLE_GM_MEMBER));
            r = scoped_post(&dave, g6, jev, &dave, "198.51.100.20:8443", me.uid);                           /* carol's join in dave's request */
            CHECK(r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_INVITED));
        }
        {
            char jev[800];
            CHECK(puddle_group_join_build(g6, dave.uid, T0 + 1, jev, sizeof jev) > 0);                      /* an answer older than the invitation */
            r = scoped_post(&dave, g6, jev, &dave, "198.51.100.20:8443", me.uid);
            CHECK(r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_INVITED));
        }
        {
            char left[53];
            CHECK(puddle_group_create(&ctx, "Not ours", left, &code) == 0);
            char jev[800];
            CHECK(puddle_group_join_build(left, dave.uid, clock_now, jev, sizeof jev) > 0);
            r = scoped_post(&dave, left, jev, &dave, "198.51.100.20:8443", me.uid);                        /* never invited to that one */
            CHECK(r->status == 202 && !in_state(left, dave.uid, PUDDLE_GM_MEMBER));
        }
        {
            char err2[200];
            CHECK(puddle_streams_block(ss, dave.uid, T0, err2, sizeof err2) == 0);
            r = scoped_post(&dave, g6, join, &dave, "198.51.100.20:8443", me.uid);                          /* blocked */
            CHECK(r->status == 404 || (r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_INVITED)));
            CHECK(puddle_streams_unblock(ss, dave.uid, err2, sizeof err2) == 0);
        }
        {
            char jev[800];
            CHECK(in_state(g1, carol.uid, PUDDLE_GM_INVITED) && my_state(g1, PUDDLE_GROUP_LEFT));
            CHECK(puddle_group_join_build(g1, carol.uid, clock_now, jev, sizeof jev) > 0);
            r = scoped_post(&carol, g1, jev, &carol, "198.51.100.21:8443", me.uid);                         /* a group we have left */
            CHECK(r->status == 202 && in_state(g1, carol.uid, PUDDLE_GM_INVITED));
        }
        /* the real thing, from someone we hold nothing of (dave's old record is a closed one) */
        r = scoped_post(&dave, g6, join, &dave, "198.51.100.20:8443", me.uid);
        CHECK(r->status == 202 && in_state(g6, dave.uid, PUDDLE_GM_MEMBER));
        {
            puddle_stream st;
            CHECK(puddle_streams_get(ss, dave.uid, &st) == 0 && st.state == PUDDLE_STREAM_SCOPED && st.group_ok && st.owed == PUDDLE_OWED_SCOPED);
            puddle_stream_free(&st);
            CHECK(queued(dave.uid, "result") == 1);
            char *b = last_body(dave.uid, "result");
            CHECK(b && strstr(b, "\"approved\":true") && strstr(b, "\"scoped\":true") && !strstr(b, "\"ticket\""));
            free(b);
        }
        /* a friend asks for the same thing: they stay a friend, and the connection is noted */
        {
            char jev[800];
            clock_now += 100;
            CHECK(puddle_group_invite_build(g6, alice.uid, clock_now, carol.uid, jev, sizeof jev) > 0);
            CHECK(has_text(deliver_one(&alice, g6, &alice, jev), "\"accepted\":[\""));
            clock_now += 100;
            CHECK(puddle_group_join_build(g6, carol.uid, clock_now, jev, sizeof jev) > 0);
            r = scoped_post(&dave, g6, jev, &dave, "198.51.100.20:8443", me.uid);                           /* dave brings carol's join, signed by dave */
            CHECK(r->status == 202 && in_state(g6, carol.uid, PUDDLE_GM_INVITED));
            r = scoped_post(&carol, g6, jev, &carol, "198.51.100.21:8443", me.uid);
            puddle_stream st;
            CHECK(r->status == 202 && in_state(g6, carol.uid, PUDDLE_GM_MEMBER) && puddle_streams_get(ss, carol.uid, &st) == 0 &&
                  st.state == PUDDLE_STREAM_ACTIVE && st.group_ok);
            puddle_stream_free(&st);
        }
        /* a group-only peer who then asks to be a friend: a request is waiting, and the group goes on */
        {
            char *acc = who_json(&dave, "requester", "198.51.100.20:8443", "Dave", "\"message\":\"be friends?\"");
            r = post_as(&dave, "/stream/v1/access", acc);
            free(acc);
            puddle_stream st;
            CHECK(r->status == 202 && puddle_streams_get(ss, dave.uid, &st) == 0 && st.state == PUDDLE_STREAM_PENDING_IN && st.group_ok && !strcmp(st.note, "be friends?"));
            CHECK(puddle_stream_carries_groups(&st));
            puddle_stream_free(&st);
            char ev[800];
            CHECK(puddle_message_build(g6, dave.uid, clock_now + 10, "still here", 10, NULL, ev, sizeof ev) > 0);
            CHECK(has_text(deliver_one(&dave, g6, &dave, ev), "\"accepted\":[\""));
        }
    }

    /* ---- we join, and connect to the members we hold nothing of ---- */
    {
        char tmp[53], ev[800];
        int64_t t = clock_now + 10000;
        char extra[600];
        snprintf(extra, sizeof extra, "{\"id\":\"%s\",\"state\":\"member\",\"ts\":%lld,\"address\":\"198.51.100.77:8443\"}", ivy.uid, (long long)T0 + 5);
        char *pk = package(&alice, &alice, NULL, "Strangers", "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", T0 + 200, t, roster(&alice, &me, extra), NULL, tmp, sizeof tmp);
        clock_now = t;
        CHECK(post_as(&alice, "/stream/v1/group/invite", pk)->status == 202);
        free(pk);
        puddle_stream st;
        CHECK(puddle_streams_get(ss, ivy.uid, &st) == 1);                                       /* before we say yes, nothing is asked of anyone */
        CHECK(puddle_group_accept(&ctx, tmp, &code) == 0);
        CHECK(puddle_streams_get(ss, ivy.uid, &st) == 0 && st.state == PUDDLE_STREAM_PENDING_OUT && st.scoped_req && !st.group_ok && st.naddr == 1 &&
              !strcmp(st.addresses[0], "198.51.100.77:8443") && st.chain == NULL);
        puddle_stream_free(&st);
        CHECK(queued(ivy.uid, "access") == 1);
        char *b = last_body(ivy.uid, "access");
        CHECK(b && strstr(b, "\"scope\":{\"group\":\"") && strstr(b, tmp) && strstr(b, "\"join\":{\"event\":\"") && strstr(b, "\"requester\":{\"id\":\"") && !strstr(b, "\"ticket\""));
        free(b);
        CHECK(queued(alice.uid, "access") == 0);                                                 /* alice is a friend: nothing to ask */
        puddle_group_reconcile(&ctx);
        CHECK(queued(ivy.uid, "access") == 1);                                                  /* and nothing is asked twice */

        /* ivy's yes to a group-only request */
        {
            char *res = who_json(&ivy, "responder", "198.51.100.77:8443", "Ivy", "\"approved\":true,\"scoped\":true");
            CHECK(post_as(&ivy, "/stream/v1/access/result", res)->status == 202);
            free(res);
            CHECK(puddle_streams_get(ss, ivy.uid, &st) == 0 && st.state == PUDDLE_STREAM_SCOPED && st.group_ok && !st.scoped_req && st.chain && strstr(their_profile(ivy.uid), "Ivy"));
            puddle_stream_free(&st);
            CHECK(puddle_group_open_with(&ctx, ivy.uid));
        }
        /* we leave: the connection that was only for this group goes with it, and friends stay friends */
        CHECK(puddle_group_leave(&ctx, tmp, &code) == 0);
        CHECK(puddle_streams_get(ss, ivy.uid, &st) == 0 && st.state == PUDDLE_STREAM_CLOSED && !st.group_ok && st.owed == PUDDLE_OWED_CLOSED);
        puddle_stream_free(&st);
        CHECK(queued(ivy.uid, "close") == 1 && !puddle_group_open_with(&ctx, ivy.uid));
        CHECK(puddle_streams_get(ss, alice.uid, &st) == 0 && st.state == PUDDLE_STREAM_ACTIVE);
        puddle_stream_free(&st);
        (void)ev;
    }
    /* a yes we did not ask for is no one's friend request */
    {
        puddle_stream st;
        CHECK(puddle_streams_get(ss, bob.uid, &st) == 0 && st.state == PUDDLE_STREAM_ACTIVE);
        puddle_stream_free(&st);
        char *res = who_json(&bob, "responder", "198.51.100.5:8443", "Bob", "\"approved\":true,\"scoped\":true");
        CHECK(post_as(&bob, "/stream/v1/access/result", res)->status == 202 && puddle_streams_get(ss, bob.uid, &st) == 0 && st.state == PUDDLE_STREAM_ACTIVE && !st.group_ok);
        puddle_stream_free(&st);
        free(res);
    }

    /* we had asked jon for a group connection, and now jon asks us for the same: that is the answer, and he is not left as a request of ours */
    {
        char ev[800], err2[200];
        puddle_stream rec;
        clock_now += 100;
        CHECK(puddle_group_invite_build(g6, alice.uid, clock_now, jon.uid, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&alice, g6, &alice, ev), "\"accepted\":[\""));    /* alice is still a member of g6 here */
        CHECK(puddle_stream_init(&rec, jon.uid, PUDDLE_STREAM_PENDING_OUT, T0) == 0 && puddle_stream_add_address(&rec, "198.51.100.95:8443") == 0);
        rec.scoped_req = 1;
        CHECK(puddle_streams_put(ss, &rec, err2, sizeof err2) == 0);
        puddle_stream_free(&rec);
        clock_now += 100;
        CHECK(puddle_group_join_build(g6, jon.uid, clock_now, ev, sizeof ev) > 0);
        reply *r = scoped_post(&jon, g6, ev, &jon, "198.51.100.95:8443", me.uid);
        CHECK(r->status == 202);
        puddle_stream st;
        CHECK(puddle_streams_get(ss, jon.uid, &st) == 0 && st.state == PUDDLE_STREAM_SCOPED && !st.scoped_req && st.group_ok);
        puddle_stream_free(&st);
    }

    /* answers that are not for what we asked */
    {
        person gus, hal;
        make(&gus, "gus"); make(&hal, "hal");
        puddle_stream rec;
        char err2[200];
        /* a friend request of ours is pending; a yes to a group-only request is not its answer */
        CHECK(puddle_stream_init(&rec, gus.uid, PUDDLE_STREAM_PENDING_OUT, T0) == 0 && puddle_stream_add_address(&rec, "198.51.100.90:8443") == 0 &&
              puddle_streams_put(ss, &rec, err2, sizeof err2) == 0);
        puddle_stream_free(&rec);
        char *res = who_json(&gus, "responder", "198.51.100.90:8443", "Gus", "\"approved\":true,\"scoped\":true");
        CHECK(post_as(&gus, "/stream/v1/access/result", res)->status == 202 && in_state_stream(gus.uid, PUDDLE_STREAM_PENDING_OUT));
        free(res);
        /* a group-only request of ours is pending; a friend's yes makes a friend of them */
        CHECK(puddle_stream_init(&rec, hal.uid, PUDDLE_STREAM_PENDING_OUT, T0) == 0 && puddle_stream_add_address(&rec, "198.51.100.91:8443") == 0);
        rec.scoped_req = 1;
        CHECK(puddle_streams_put(ss, &rec, err2, sizeof err2) == 0);
        puddle_stream_free(&rec);
        res = who_json(&hal, "responder", "198.51.100.91:8443", "Hal", "\"approved\":true");
        CHECK(post_as(&hal, "/stream/v1/access/result", res)->status == 202 && in_state_stream(hal.uid, PUDDLE_STREAM_ACTIVE));
        free(res);
    }

    /* ---- leaving the group they were in for: friends stay friends, a request stays a request ---- */
    {
        char ev[800];
        /* erin joins g6 too, then loses her stream; as the creator we do not ask her again: we hold no join to show */
        clock_now += 100;
        CHECK(puddle_group_join_build(g6, erin.uid, clock_now, ev, sizeof ev) > 0);
        reply *r = scoped_post(&erin, g6, ev, &erin, "198.51.100.77:8443", me.uid);
        CHECK(r->status == 202 && in_state(g6, erin.uid, PUDDLE_GM_MEMBER) && puddle_group_open_with(&ctx, erin.uid));
        puddle_stream st;
        char err2[200];
        CHECK(puddle_streams_get(ss, erin.uid, &st) == 0);
        st.state = PUDDLE_STREAM_CLOSED;
        st.group_ok = 0;
        CHECK(puddle_streams_put(ss, &st, err2, sizeof err2) == 0);
        puddle_stream_free(&st);
        int asked = queued(erin.uid, "access");
        puddle_group_reconcile(&ctx);
        CHECK(queued(erin.uid, "access") == asked);
        /* we leave g6: a friend who was in it stays a friend; someone who had asked to be one is still asking */
        CHECK(puddle_streams_get(ss, carol.uid, &st) == 0 && st.group_ok);
        puddle_stream_free(&st);
        CHECK(puddle_group_leave(&ctx, g6, &code) == 0);
        CHECK(puddle_streams_get(ss, carol.uid, &st) == 0 && st.state == PUDDLE_STREAM_ACTIVE && !st.group_ok);
        puddle_stream_free(&st);
        CHECK(puddle_streams_get(ss, dave.uid, &st) == 0 && st.state == PUDDLE_STREAM_PENDING_IN && st.group_ok);      /* g2 is still shared with dave */
        puddle_stream_free(&st);
        CHECK(puddle_group_open_with(&ctx, dave.uid) && puddle_group_open_with(&ctx, carol.uid)); /* a friend is always open */
        CHECK(puddle_group_leave(&ctx, g2, &code) == 0);                                                              /* and now it is not */
        CHECK(puddle_streams_get(ss, dave.uid, &st) == 0 && st.state == PUDDLE_STREAM_PENDING_IN && !st.group_ok);
        puddle_stream_free(&st);
        CHECK(!puddle_group_open_with(&ctx, dave.uid));
    }

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
