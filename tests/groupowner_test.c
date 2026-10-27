#include "federation/groupapi.h"
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
static person me, alice, bob, carol, dave, erin;
static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
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

static reply *call_m(const char *method, const char *path, const char *body, const char *sig, int owner);
static reply *call(const char *path, const char *body, const char *sig)
{ return call_m("POST", path, body, sig, 0); }
static reply *call_m(const char *method, const char *path, const char *body, const char *sig, int owner)
{
    static reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", path);
    snprintf(req.path, sizeof req.path, "%s", path);
    { char *q = strchr(req.path, '?'); if (q) *q = 0; }
    if (sig) snprintf(req.signature, sizeof req.signature, "%s", sig);
    size_t bl = strlen(body);
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    memcpy(copy, body, bl + 1);
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
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

static int in_state(const char *gid, const char *who, puddle_gm_state want)
{
    puddle_group g;
    if (puddle_groups_get(gs, gid, &g) != 0) return 0;
    puddle_gmember *m = puddle_group_member(&g, who);
    int ok = m && m->state == want;
    puddle_group_free(&g);
    return ok;
}


static reply *own(const char *method, const char *path, const char *body) { return call_m(method, path, body ? body : "", NULL, 1); }
static int count_in(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-groupowner-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&bob, "bob"); make(&carol, "carol"); make(&dave, "dave"); make(&erin, "erin");
    char err[300], data[700];
    snprintf(data, sizeof data, "%s/data", root);
    store = puddle_store_open(data, err, sizeof err);
    ss = puddle_streams_open(data, err, sizeof err);
    gs = puddle_groups_open(data, err, sizeof err);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .identity = me.id, .store = store, .streams = ss, .groups = gs, .outbox = ob, .replay = puddle_replay_new(4096), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    set_peer(&alice, PUDDLE_STREAM_ACTIVE);
    set_peer(&bob, PUDDLE_STREAM_ACTIVE);
    set_peer(&dave, PUDDLE_STREAM_SCOPED);
    reply *r;
    char body[1500], gid[53], url[300];

    /* nothing is open to anyone but the owner */
    CHECK(call_m("GET", "/api/v1/groups", "", NULL, 0)->status == 404);
    CHECK(call_m("POST", "/api/v1/groups/create", "{\"name\":\"x\"}", NULL, 0)->status == 404);
    CHECK(own("GET", "/api/v1/groups", NULL)->status == 200 && has_text(own("GET", "/api/v1/groups", NULL), "\"items\":[]"));

    /* ---- create ---- */
    CHECK(own("POST", "/api/v1/groups/create", "{}")->status == 400);
    CHECK(own("POST", "/api/v1/groups/create", "{\"name\":\"\"}")->status == 400);
    CHECK(own("POST", "/api/v1/groups/create", "{\"name\":\"x\",\"members\":\"alice\"}")->status == 400);
    snprintf(body, sizeof body, "{\"name\":\"Weekend\",\"members\":[\"%s\",\"%s\",\"nope\",\"%s\"]}", alice.uid, dave.uid, erin.uid);
    r = own("POST", "/api/v1/groups/create", body);
    CHECK(r->status == 201 && has_text(r, "\"group\":{\"id\":\"") && has_text(r, "\"name\":\"Weekend\"") && has_text(r, "\"my\":\"in\"") &&
          has_text(r, "\"creator\":{\"id\":\"") && has_text(r, alice.uid));
    CHECK(has_text(r, "\"not_invited\":[{\"user\":\"") && has_text(r, "\"reason\":\"no_stream\"") && count_in(body_of(r), "no_stream") == 2); /* dave is no friend; erin is no one */
    {
        const char *at = strstr(body_of(r), "\"id\":\"");
        memcpy(gid, at + 6, 52);
        gid[52] = 0;
        CHECK(puddle_id_valid(gid) && in_state(gid, alice.uid, PUDDLE_GM_INVITED) && queued(alice.uid, "group_invite") == 1);
    }
    r = own("GET", "/api/v1/groups", NULL);
    CHECK(r->status == 200 && has_text(r, gid) && has_text(r, "\"state\":\"invited\"") && has_text(r, "\"state\":\"member\"") &&
          has_text(r, "\"members\":[{\"user\":{\"id\":\"") && has_text(r, "\"since\":"));
    CHECK(own("GET", "/api/v1/groups?limit=0", NULL)->status == 400 && own("GET", "/api/v1/groups?offset=x", NULL)->status == 400);
    CHECK(own("POST", "/api/v1/groups", "{}")->status == 405);
    char gpath[200], cpath[200];
    snprintf(gpath, sizeof gpath, "/api/v1/groups/%s", gid);
    snprintf(cpath, sizeof cpath, "/api/v1/conversations/%s", gid);
#define G(verb) (snprintf(url, sizeof url, "%s/%s", gpath, verb), url)
#define C(rest) (snprintf(url, sizeof url, "%s/%s", cpath, rest), url)
    r = own("GET", gpath, NULL);
    CHECK(r->status == 200 && has_text(r, "\"name\":\"Weekend\""));

    /* alice joins (an event, as she is a friend) */
    {
        char ev[300];
        clock_now += 1000;
        CHECK(puddle_group_join_build(gid, alice.uid, clock_now, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&alice, gid, &alice, ev), "\"accepted\":[\""));
    }

    /* ---- invite, rename, leave, accept, decline ---- */
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", bob.uid);
    r = own("POST", G("invite"), body);
    CHECK(r->status == 200 && has_text(r, bob.uid) && in_state(gid, bob.uid, PUDDLE_GM_INVITED));
    CHECK(own("POST", G("invite"), body)->status == 409);
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", dave.uid);
    CHECK(own("POST", G("invite"), body)->status == 403);
    CHECK(own("POST", G("invite"), "{\"user\":\"y\"}")->status == 400);
    CHECK(own("POST", "/api/v1/groups/x/invite", "{\"user\":\"y\"}")->status == 404);
    r = own("POST", G("rename"), "{\"name\":\"Weekend away\"}");
    CHECK(r->status == 200 && has_text(r, "\"name\":\"Weekend away\""));
    CHECK(own("POST", G("rename"), "{\"name\":\"\"}")->status == 400);
    CHECK(own("POST", G("accept"), NULL)->status == 409 && own("POST", G("decline"), NULL)->status == 409);   /* we are in it, not invited */
    CHECK(own("POST", "/api/v1/groups/eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/accept", NULL)->status == 404);

    /* ---- messages ---- */
    CHECK(own("POST", "/api/v1/conversations/x/messages/send", "{\"body\":\"y\"}")->status == 404);
    r = own("POST", C("messages/send"), "{\"body\":\"  hello group  \"}");
    CHECK(r->status == 201 && has_text(r, "\"body\":\"  hello group  \"") && has_text(r, "\"outgoing\":true") && has_text(r, gid));
    char first[65];
    {
        const char *at = strstr(body_of(r), "\"id\":\"");
        memcpy(first, at + 6, 64);
        first[64] = 0;
    }
    CHECK(queued(alice.uid, "events") >= 1);
    snprintf(body, sizeof body, "{\"body\":\"second\",\"reply_to\":\"%s\"}", first);
    r = own("POST", C("messages/send"), body);
    CHECK(r->status == 201 && has_text(r, "\"reply_to\":\"") && has_text(r, first));
    CHECK(own("POST", C("messages/send"), "{\"body\":\"\"}")->status == 400);
    CHECK(own("POST", C("messages/send"), "{\"body\":\"bad\\u0007\"}")->status == 400);
    CHECK(own("POST", "/api/v1/conversations/eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/messages/send", "{\"body\":\"hi\"}")->status == 404);
    /* a member's message arrives */
    {
        char ev[800];
        clock_now += 1000;
        CHECK(puddle_message_build(gid, alice.uid, clock_now, "hi from alice", 13, NULL, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&alice, gid, &alice, ev), "\"accepted\":[\""));
    }
    r = own("GET", C("messages"), NULL);
    CHECK(r->status == 200 && has_text(r, "\"items\":[{") && has_text(r, "hello group") && has_text(r, "hi from alice") && has_text(r, "\"next\":null"));
    CHECK(has_text(r, "\"delivery\":{\"state\":\"queued\""));                                 /* ours has not gone yet */
    /* what happened to the group is in the same list, in its place */
    CHECK(has_text(r, "\"type\":\"group_create\"") && has_text(r, "\"type\":\"group_invite\"") && has_text(r, "\"type\":\"group_join\"") &&
          has_text(r, "\"type\":\"group_rename\"") && has_text(r, "\"name\":\"Weekend away\""));
    CHECK(strstr(body_of(r), "\"type\":\"group_create\"") < strstr(body_of(r), "\"type\":\"group_join\""));
    CHECK(strstr(body_of(r), "\"type\":\"group_rename\"") < strstr(body_of(r), "hello group"));
    r = own("GET", C("messages?limit=1"), NULL);
    CHECK(r->status == 200 && count_in(body_of(r), "\"type\":\"default\"") == 1 && has_text(r, "hi from alice") && !has_text(r, "\"next\":null"));
    CHECK(!has_text(r, "\"type\":\"group_create\""));                                         /* an older page's notices are not here */
    CHECK(own("GET", "/api/v1/conversations/nope/messages", NULL)->status == 404);
    CHECK(own("GET", "/api/v1/conversations/eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/messages", NULL)->status == 404);
    CHECK(own("GET", C("messages?limit=0"), NULL)->status == 400);

    /* ---- edit and delete ---- */
    char mpath[300];
    snprintf(mpath, sizeof mpath, "messages/%s/edit", first);
    r = own("POST", C(mpath), "{\"body\":\"hello again\"}");
    CHECK(r->status == 200 && has_text(r, "\"body\":\"hello again\"") && has_text(r, "\"edited_at\":"));
    r = own("GET", C("messages"), NULL);
    CHECK(has_text(r, "hello again") && !has_text(r, "hello group"));
    {
        /* someone else's message is not ours to touch */
        const char *at = strstr(body_of(r), "hi from alice");
        char theirs[65] = "";
        for (const char *q = at; q > body_of(r); q--)
            if (!strncmp(q, "{\"id\":\"", 7) && !strncmp(q + 71, "\",\"conversation\"", 16)) { memcpy(theirs, q + 7, 64); break; }
        CHECK(strlen(theirs) == 64);
        snprintf(mpath, sizeof mpath, "messages/%s/edit", theirs);
        CHECK(own("POST", C(mpath), "{\"body\":\"mine\"}")->status == 404);
        snprintf(mpath, sizeof mpath, "messages/%s/delete", theirs);
        CHECK(own("POST", C(mpath), NULL)->status == 404);
    }
    snprintf(mpath, sizeof mpath, "messages/%s/delete", first);
    r = own("POST", C(mpath), NULL);
    CHECK(r->status == 200 && has_text(r, "\"deleted\":true"));
    CHECK(own("POST", C(mpath), NULL)->status == 200);                                       /* again: nothing more to say */
    snprintf(mpath, sizeof mpath, "messages/%s/edit", first);
    CHECK(own("POST", C(mpath), "{\"body\":\"too late\"}")->status == 409);
    CHECK(own("POST", C("messages/short/edit"), "{\"body\":\"x\"}")->status == 404);
    r = own("GET", C("messages"), NULL);
    CHECK(has_text(r, "\"deleted\":true") && !has_text(r, "hello again"));

    /* ---- what became of a message that went to several: the worst of it ---- */
    {
        char ev[300];
        clock_now += 1000;
        CHECK(puddle_group_join_build(gid, bob.uid, clock_now, ev, sizeof ev) > 0);
        CHECK(has_text(deliver_one(&bob, gid, &bob, ev), "\"accepted\":[\""));
        CHECK(own("POST", C("messages/send"), "{\"body\":\"to both\"}")->status == 201);
        puddle_outbox_poll(ob);                                                                 /* no way to reach anyone: both fail */
        r = own("GET", C("messages"), NULL);
        CHECK(has_text(r, "\"delivery\":{\"state\":\"failed\""));
        snprintf(url, sizeof url, "/api/v1/conversations/%s/retry", alice.uid);
        CHECK(own("POST", url, NULL)->status == 200);                                          /* alice's goes again; bob's has still failed */
        r = own("GET", C("messages"), NULL);
        {
            const char *at = strstr(body_of(r), "to both");
            CHECK(at && strstr(at, "\"state\":\"failed\""));
            CHECK(!strstr(at, "\"state\":\"queued\"") || strstr(at, "\"state\":\"failed\"") < strstr(at, "\"state\":\"queued\""));
        }
    }

    /* ---- the conversation list has the group, with what is unread ---- */
    r = own("GET", "/api/v1/conversations", NULL);
    CHECK(r->status == 200 && has_text(r, "\"kind\":\"group\"") && has_text(r, "\"name\":\"Weekend away\"") && has_text(r, "\"member_count\":3") &&
          has_text(r, "\"unread\":1"));
    r = own("POST", C("read"), NULL);
    CHECK(r->status == 200 && has_text(r, "\"unread\":0"));
    r = own("GET", "/api/v1/conversations", NULL);
    CHECK(has_text(r, "\"unread\":0"));
    /* a group nobody has spoken in yet is there too */
    CHECK(own("POST", "/api/v1/groups/create", "{\"name\":\"Quiet\"}")->status == 201);
    r = own("GET", "/api/v1/conversations", NULL);
    CHECK(has_text(r, "\"name\":\"Quiet\"") && count_in(body_of(r), "\"kind\":\"group\"") == 2);

    /* ---- leave ---- */
    r = own("POST", G("leave"), NULL);
    CHECK(r->status == 200 && has_text(r, "\"my\":\"left\"") && own("POST", G("leave"), NULL)->status == 409);
    r = own("GET", "/api/v1/conversations", NULL);
    CHECK(!has_text(r, "Weekend away"));                                                       /* a group we left is no conversation */
    r = own("GET", "/api/v1/groups", NULL);
    CHECK(!has_text(r, "Weekend away") && has_text(r, "Quiet"));
    CHECK(own("POST", C("messages/send"), "{\"body\":\"gone\"}")->status == 409);
#undef G
#undef C

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
