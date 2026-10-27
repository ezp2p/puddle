#include "federation/messages.h"
#include "core/conversations.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "events/message.h"
#include "core/phrase.h"
#include "service/routes.h"
#include "federation/courier.h"
#include "federation/sender.h"
#include "http/client.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL
static char root[512], data[700];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

typedef struct { puddle_identity *id; char uid[PUDDLE_USER_ID_LEN + 1]; char phrase[PUDDLE_PHRASE_LEN + 1]; } person;
static person me, alice, carol;
static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
static puddle_store *store;
static puddle_conversations *cs;

/* when the owner last read a conversation, as its record holds it */
static int64_t read_at(const char *id)
{
    puddle_conv_record rec;
    int fresh;
    return puddle_conversations_get(cs, id, &rec, &fresh) == 0 ? rec.read_at : 0;
}
static puddle_outbox *ob;
static int g_addr_mode; /* 0: we know no address for anyone; 1: a closed port on this machine (so: offline) */
static int addr_cb(void *u, const char *peer, puddle_address *out, int max)
{
    (void)u; (void)peer;
    return g_addr_mode && max > 0 && puddle_address_parse("127.0.0.1:1", &out[0]) == 0 ? 1 : 0;
}

static void make(person *p, const char *name)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, p->phrase, err, sizeof err);
    if (!p->id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

static void roll(person *p)
{
    char fresh[PUDDLE_PHRASE_LEN + 1], err[300];
    puddle_roll *r = puddle_identity_roll_prepare(p->id, p->phrase, fresh, err, sizeof err);
    if (!r || puddle_identity_roll_commit(p->id, r, err, sizeof err) != 0) exit(2);
    memcpy(p->phrase, fresh, sizeof p->phrase);
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

static reply *call(const char *method, const char *target, const char *body, const char *sig, int owner)
{
    static reply rp;
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
    unsigned char *copy = malloc(bl + 1);
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    return &rp;
}

static const char *body_of(const reply *r) { const char *b = strstr(r->out, "\r\n\r\n"); return b ? b + 4 : ""; }

static reply *owner(const char *method, const char *target, const char *body) { return call(method, target, body, NULL, 1); }

/* The owner API names the conversation in the path: "/api/v1/conversations/<who>/<rest>". `dm_to` is who the body just
 * written is for, and `dm_msg` the message an edit or delete names. */
static const char *dm_to, *dm_msg;
static const char *conv_path(const char *who, const char *rest)
{
    static char buf[4][400];
    static int k;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "/api/v1/conversations/%s%s%s", who, rest ? "/" : "", rest ? rest : "");
    return buf[k];
}
static reply *dm_send(const char *to, const char *body) { return owner("POST", conv_path(to, "messages/send"), body); }
static reply *dm_edit(const char *to, const char *msg, const char *body)
{
    char rest[100];
    snprintf(rest, sizeof rest, "messages/%s/edit", msg);
    return owner("POST", conv_path(to, rest), body);
}
static reply *dm_delete(const char *to, const char *msg)
{
    char rest[100];
    snprintf(rest, sizeof rest, "messages/%s/delete", msg);
    return owner("POST", conv_path(to, rest), NULL);
}

static reply *peer_post(person *from, const char *body)
{
    char sig[800];
    if (puddle_reqsig_sign(from->id, "POST", "/stream/v1/events", body, strlen(body), me.uid, clock_now, sig, sizeof sig) != 0) exit(2);
    return call("POST", "/stream/v1/events", body, sig, 0);
}

/* an envelope: {"id":..,"event":"<escaped>","sig":..} */
typedef struct { char id[65]; char *event; char sig[PUDDLE_SIG_TEXT_MAX]; } env;

/* just the one conversation's object out of a list, so a check cannot match its neighbour */
static const char *conv_of(const reply *r, const char *uid)
{
    static char one[1200];
    char key[80];
    snprintf(key, sizeof key, "{\"id\":\"%s\",\"kind\"", uid);
    const char *at = strstr(body_of(r), key);
    if (!at) return NULL;
    const char *end = strstr(at + strlen(key), ",\"kind\":\"");  /* the next conversation */
    size_t n = end ? (size_t)(end - at) : strlen(at);
    if (n >= sizeof one) n = sizeof one - 1;
    memcpy(one, at, n);
    one[n] = '\0';
    return one;
}

static char chat_with_me(person *p, char *out) { puddle_dm_chat_id(me.uid, p->uid, out); return 0; }

static env make_env(person *signer, person *claimed_sender, const char *chat, int64_t ts, const char *body, const char *ref)
{
    env e;
    char ev[PUDDLE_EVENT_MAX_BYTES + 100];
    size_t el = puddle_message_build(chat, claimed_sender->uid, ts, body, strlen(body), ref, ev, sizeof ev);
    if (!el) { fprintf(stderr, "cannot build\n"); exit(2); }
    e.event = strdup(ev);
    puddle_sha256_hex(ev, el, e.id);
    puddle_event_sign(signer->id, ev, el, e.sig);
    return e;
}

/* {"chat":"<chat>","events":[...]} */
/* an envelope for an event whose bytes are already made */
static env env_of(person *signer, const char *event)
{
    env e;
    e.event = strdup(event);
    puddle_sha256_hex(event, strlen(event), e.id);
    puddle_event_sign(signer->id, event, strlen(event), e.sig);
    return e;
}

static char *delivery(const char *chat, env *es, size_t n)
{
    size_t cap = 1 << 20;
    char *buf = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_key(&w, "events");
    puddle_jw_array(&w);
    for (size_t i = 0; i < n; i++) {
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", es[i].id);
        puddle_jw_str(&w, "event", es[i].event);
        puddle_jw_str(&w, "sig", es[i].sig);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    return buf;
}

static int count_in(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
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

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-messages-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&carol, "carol");
    char err[300];
    snprintf(data, sizeof data, "%s/data", root);
    store = puddle_store_open(data, err, sizeof err);
    cs = puddle_conversations_open(data, err, sizeof err);
    puddle_store_on_append(store, puddle_conversations_appended, cs);
    ss = puddle_streams_open(data, err, sizeof err);
    puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .identity = me.id, .store = store, .streams = ss, .conversations = cs, .outbox = ob, .replay = puddle_replay_new(1024), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);

    char chat_a[64], chat_c[64];
    chat_with_me(&alice, chat_a);
    chat_with_me(&carol, chat_c);
    char chat_ac[64];
    puddle_dm_chat_id(alice.uid, carol.uid, chat_ac);
    reply *r;
    char *b;
    env e1;

    /* ---- a peer delivers ---- */
    set_peer(&alice, PUDDLE_STREAM_ACTIVE);
    e1 = make_env(&alice, &alice, chat_a, T0 + 10, "hello Bob", NULL);
    b = delivery(chat_a, &e1, 1);
    r = peer_post(&alice, b);
    CHECK(r->status == 200 && strstr(body_of(r), e1.id) && strstr(body_of(r), "\"accepted\":[\"") && !strstr(body_of(r), "\"duplicate\":[\""));
    CHECK(stored(chat_a) == 1);
    {
        puddle_event ev;
        CHECK(puddle_store_get(store, chat_a, e1.id, &ev, err, sizeof err) == 0);
        CHECK(!strcmp(ev.event, e1.event) && !strcmp(ev.sig, e1.sig));                          /* kept exactly as signed */
        puddle_event_free(&ev);
    }
    {
        /* the very same request sent twice is a replay, whatever it carries */
        char sig[800];
        puddle_reqsig_sign(alice.id, "POST", "/stream/v1/events", b, strlen(b), me.uid, clock_now, sig, sizeof sig);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 200);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 401);
    }
    clock_now += 1000;
    r = peer_post(&alice, b);                                                                    /* the same message in a new request */
    CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[]") && strstr(body_of(r), "\"duplicate\":[\"") && stored(chat_a) == 1);
    free(b);

    /* a delivery of several, some bad: each is judged alone */
    {
        clock_now += 1000;
        env es[14];
        int n = 0;
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 20, "second", NULL);                    /* 0 good */
        es[n++] = make_env(&alice, &carol, chat_a, T0 + 21, "as Carol", NULL);                  /* 1 wrong sender */
        es[n++] = make_env(&alice, &alice, chat_ac, T0 + 22, "wrong chat", NULL);               /* 2 wrong chat */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 23, "tampered id", NULL);                 /* 3 id is not the hash */
        es[n].id[0] = es[n].id[0] == 'a' ? 'b' : 'a';
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 24, "tampered body", NULL);               /* 4 bytes changed after signing */
        { char *p = strstr(es[n].event, "tampered body"); p[0] = 'T'; puddle_sha256_hex(es[n].event, strlen(es[n].event), es[n].id); }
        n++;
        es[n++] = make_env(&carol, &alice, chat_a, T0 + 25, "forged", NULL);                    /* 5 signed by someone else */
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 26, "x", NULL);                         /* 6 good (placeholder replaced below) */
        { free(es[n - 1].event); es[n - 1].event = strdup("{\"type\":\"chat.create\",\"chat\":\"x\",\"sender\":\"y\",\"ts\":1,\"body\":\"b\"}");
          puddle_sha256_hex(es[n - 1].event, strlen(es[n - 1].event), es[n - 1].id);
          puddle_event_sign(alice.id, es[n - 1].event, strlen(es[n - 1].event), es[n - 1].sig); }  /* 6 a type we do not take */
        es[n++] = make_env(&alice, &alice, chat_a, 0, "no time", NULL);                         /* 7 bad time */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 28, "bad sig", NULL);                     /* 8 garbage signature */
        snprintf(es[n].sig, sizeof es[n].sig, "%.64s:AAAA", es[n].sig);
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 29, "no sig", NULL);                      /* 9 no signature at all */
        es[n].sig[0] = 0;
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 30, "other key", NULL);                   /* 10 a key that is not theirs */
        {
            char kid[65];
            unsigned char cp[32];
            puddle_identity_public(carol.id, cp, NULL, NULL);
            puddle_key_id(cp, kid);
            snprintf(es[n].sig, sizeof es[n].sig, "%s%s", kid, strchr(es[n].sig, ':'));
        }
        n++;
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 31, "third", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"); /* 11 good, a reply */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 32, "dup in same delivery", NULL);        /* 12 and 13: the same twice */
        n++;
        es[n] = es[n - 1];
        es[n].event = strdup(es[n - 1].event);
        n++;
        b = delivery(chat_a, es, (size_t)n);
        r = peer_post(&alice, b);
        CHECK(r->status == 200);
        const char *o = body_of(r);
        CHECK(count_in(o, "\"reason\"") == 10);
        const char *whys[] = { "wrong_sender", "wrong_chat", "bad_id", "bad_id", "unknown_key", "unsupported_type", "bad_time", "bad_signature", "bad_signature" };
        (void)whys;
        CHECK(strstr(o, "wrong_sender") && strstr(o, "wrong_chat") && strstr(o, "bad_id") && strstr(o, "unknown_key") && strstr(o, "unsupported_type") &&
              strstr(o, "bad_time") && strstr(o, "bad_signature"));
        CHECK(count_in(o, "\"duplicate\":[\"") == 1 && strstr(o, es[13].id));                   /* the repeat inside a delivery is a duplicate */
        CHECK(strstr(o, es[0].id) && strstr(o, es[11].id) && strstr(o, es[12].id));
        CHECK(stored(chat_a) == 4);                                                             /* hello, second, third, the one repeated */
        for (int i = 0; i < n; i++) free(es[i].event);
        free(b);
    }

    /* the delivery as a whole */
    {
        clock_now += 1000;
        e1 = make_env(&alice, &alice, chat_a, T0 + 40, "x", NULL);
        b = delivery(chat_c, &e1, 1);
        CHECK(peer_post(&alice, b)->status == 400);                                              /* a chat that is not yours with us */
        free(b);
        clock_now += 1000;
        CHECK(peer_post(&alice, "{\"chat\":\"nonsense\",\"events\":[]}")->status == 400);
        clock_now += 1000;
        CHECK(peer_post(&alice, "not json")->status == 400);
        clock_now += 1000;
        char t[400];
        snprintf(t, sizeof t, "{\"chat\":\"%s\"}", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);                                              /* no events */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":{}}", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);                                              /* not a list */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[1,2,", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[]}", chat_a);
        r = peer_post(&alice, t);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[]"));                       /* nothing is fine */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[1,\"x\",[],null,{}]}", chat_a);
        r = peer_post(&alice, t);
        CHECK(r->status == 200 && count_in(body_of(r), "\"reason\":\"malformed\"") == 5);        /* junk entries are refused one by one */
        clock_now += 1000;
        env many[PUDDLE_DELIVERY_MAX_EVENTS + 1];
        for (int i = 0; i <= PUDDLE_DELIVERY_MAX_EVENTS; i++) many[i] = make_env(&alice, &alice, chat_a, T0 + 1000 + i, "bulk", NULL);
        b = delivery(chat_a, many, PUDDLE_DELIVERY_MAX_EVENTS + 1);
        CHECK(peer_post(&alice, b)->status == 400);                                              /* over the limit: refused whole */
        CHECK(stored(chat_a) == 4);
        free(b);
        clock_now += 1000;
        b = delivery(chat_a, many, PUDDLE_DELIVERY_MAX_EVENTS);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && count_in(body_of(r), "\"reason\"") == 0 && stored(chat_a) == 4 + 100);   /* the same words, a hundred times: each is its own message */
        for (int i = 0; i <= PUDDLE_DELIVERY_MAX_EVENTS; i++) free(many[i].event);
        free(b);
    }

    /* an event that is too large for one message */
    {
        clock_now += 1000;
        env e;
        e.event = malloc(PUDDLE_EVENT_MAX_BYTES + 10);
        memset(e.event, 'x', PUDDLE_EVENT_MAX_BYTES + 5);
        e.event[0] = '{';
        e.event[PUDDLE_EVENT_MAX_BYTES + 4] = '}';
        e.event[PUDDLE_EVENT_MAX_BYTES + 5] = 0;
        puddle_sha256_hex(e.event, strlen(e.event), e.id);
        snprintf(e.sig, sizeof e.sig, "x");
        b = delivery(chat_a, &e, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "too_large"));
        free(b); free(e.event);
    }

    /* ---- who may deliver ---- */
    {
        clock_now += 1000;
        e1 = make_env(&carol, &carol, chat_c, T0 + 50, "hi", NULL);
        b = delivery(chat_c, &e1, 1);
        char sig[800];
        puddle_reqsig_sign(carol.id, "POST", "/stream/v1/events", b, strlen(b), me.uid, clock_now, sig, sizeof sig);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 404);                      /* a stranger: no such route */
        set_peer(&carol, PUDDLE_STREAM_PENDING_IN);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 403 && strstr(body_of(call("POST", "/nothing", "{}", NULL, 0)), "Not Found"));  /* known, but no open stream */
        reply *rr = peer_post(&carol, b);
        clock_now += 1000;
        rr = peer_post(&carol, b);
        CHECK(rr->status == 403 && strstr(body_of(rr), "no_stream"));
        set_peer(&carol, PUDDLE_STREAM_CLOSED);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 403);
        CHECK(stored(chat_c) == 0);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 404);                                              /* blocked: as if not there */
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 200 && stored(chat_c) == 1);
        free(b);
        /* Alice cannot deliver into Carol's chat with us, nor Carol into Alice's */
        e1 = make_env(&alice, &alice, chat_c, T0 + 51, "into carol's chat", NULL);
        b = delivery(chat_c, &e1, 1);
        clock_now += 1000;
        CHECK(peer_post(&alice, b)->status == 400 && stored(chat_c) == 1);
        free(b);
    }

    /* ---- after a key change ---- */
    {
        roll(&alice);                                                                             /* we have not been told yet */
        clock_now += 1000;
        e1 = make_env(&alice, &alice, chat_a, T0 + 60, "after the roll", NULL);
        b = delivery(chat_a, &e1, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 404);                                                                  /* her new key is not one we hold: not her */
        free(b);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE);                                                   /* the news arrives (4.2 will do this) */
        clock_now += 1000;
        b = delivery(chat_a, &e1, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\""));
        free(b);
    }

    /* ---- the owner sends ---- */
    {
        size_t before_chat = stored(chat_a);
        char body[1200], cs[64];
        snprintf(body, sizeof body, "{\"body\":\"hi Alice\"}"); dm_to = alice.uid;
        CHECK(call("POST", conv_path(dm_to, "messages/send"), body, NULL, 0)->status == 404);                    /* not the owner */
        clock_now += 1000;
        r = dm_send(dm_to, body);
        CHECK(r->status == 201 && strstr(body_of(r), "\"body\":\"hi Alice\"") && strstr(body_of(r), "\"outgoing\":true") && strstr(body_of(r), "\"conversation\"") && strstr(body_of(r), alice.uid));
        CHECK(stored(chat_a) == before_chat + 1);
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 1 && !strcmp(l[0].kind, "events") && !strcmp(l[0].path, "/stream/v1/events") && !strcmp(l[0].peer, alice.uid));
        {
            /* what is queued is what the other side will accept: the id is the hash, and the signature is ours */
            puddle_span chat_v, evs, ev, id_v, event_v, sig_v;
            size_t cur = 0;
            CHECK(puddle_json_member(l[0].body, l[0].body_len, "events", &evs) == 0 && puddle_json_array_next(evs, &cur, &ev) == 1);
            CHECK(puddle_json_member(ev.p, ev.len, "id", &id_v) == 0 && puddle_json_member(ev.p, ev.len, "event", &event_v) == 0 &&
                  puddle_json_member(ev.p, ev.len, "sig", &sig_v) == 0);
            char idt[80], sigt[PUDDLE_SIG_TEXT_MAX], *evt = malloc(event_v.len + 1), hash[65], kid[65];
            unsigned char sg[64], mp[32];
            CHECK(puddle_json_string(id_v, idt, sizeof idt) == 0 && puddle_json_string(event_v, evt, event_v.len + 1) == 0 &&
                  puddle_json_string(sig_v, sigt, sizeof sigt) == 0);
            puddle_sha256_hex(evt, strlen(evt), hash);
            CHECK(!strcmp(hash, idt) && puddle_event_sig_parse(sigt, kid, sg) == 0);
            puddle_identity_public(me.id, mp, NULL, NULL);
            CHECK(puddle_event_verify(mp, evt, strlen(evt), sg));
            CHECK(puddle_json_member(l[0].body, l[0].body_len, "chat", &chat_v) == 0);
            free(evt);
        }
        puddle_outbox_list_free(l, n);

        /* the same words twice are two messages */
        r = dm_send(dm_to, body);
        CHECK(r->status == 201 && stored(chat_a) == before_chat + 2);
        /* a reply */
        char first_id[80] = "";
        {
            puddle_event_list tail;
            puddle_store_tail(store, chat_a, 1, &tail, err, sizeof err);
            snprintf(first_id, sizeof first_id, "%s", tail.items[0].id);
            puddle_event_list_free(&tail);
        }
        snprintf(body, sizeof body, "{\"body\":\"re\",\"reply_to\":\"%s\"}", first_id); dm_to = alice.uid;
        r = dm_send(dm_to, body);
        CHECK(r->status == 201 && strstr(body_of(r), first_id) && strstr(body_of(r), "\"reply_to\""));

        /* what it refuses */
        snprintf(cs, sizeof cs, "%s", carol.uid);
        struct { const char *to; const char *body; int status; const char *code; } bad[8] = {
            { alice.uid, "{\"body\":\"\"}", 400, "bad_body" },
            { alice.uid, "{\"body\":\"bell\\u0007\"}", 400, "bad_body" },
            { alice.uid, "{}", 400, "bad_request" },
            { me.uid, "{\"body\":\"x\"}", 400, "cannot_message_yourself" },
            { alice.uid, "{\"body\":\"x\",\"reply_to\":\"nope\"}", 400, "bad_request" },
            { alice.uid, "{\"body\":5}", 400, "bad_request" },
            { "junk", "{\"body\":\"x\"}", 404, "" },                                            /* not an id: no such path */
            { alice.uid, "nonsense", 400, "bad_request" },
        };
        size_t after = stored(chat_a);
        for (int i = 0; i < 8; i++) {
            r = dm_send(bad[i].to, bad[i].body);
            if (r->status != bad[i].status || !strstr(body_of(r), bad[i].code)) { fprintf(stderr, "FAIL send refusal %d: %d %s\n", i, r->status, body_of(r)); failures++; }
        }
        {
            char big[4200 + 100];
            snprintf(big, sizeof big, "{\"body\":\"");
            size_t k = strlen(big);
            memset(big + k, 'x', 4001);
            strcpy(big + k + 4001, "\"}");
            CHECK(dm_send(alice.uid, big)->status == 400);
            memset(big + k, 'x', 4000);
            strcpy(big + k + 4000, "\"}");
            CHECK(dm_send(alice.uid, big)->status == 201);                                         /* exactly 4000 */
            after++;
        }
        CHECK(stored(chat_a) == after);                                                           /* nothing refused was kept */
        /* only to someone we have an open stream with */
        snprintf(body, sizeof body, "{\"body\":\"x\"}"); dm_to = cs;
        set_peer(&carol, PUDDLE_STREAM_CLOSED);
        CHECK(dm_send(dm_to, body)->status == 403 && strstr(body_of(r = dm_send(dm_to, body)), "no_stream"));
        set_peer(&carol, PUDDLE_STREAM_PENDING_OUT);
        CHECK(dm_send(dm_to, body)->status == 403);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        CHECK(dm_send(dm_to, body)->status == 403);
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        char stranger[64];
        snprintf(stranger, sizeof stranger, "%s", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        snprintf(body, sizeof body, "{\"body\":\"x\"}"); dm_to = stranger;
        CHECK(dm_send(dm_to, body)->status == 404);                                               /* no stream at all: no conversation */
        CHECK(stored(chat_c) == 1);
    }

    /* ---- the owner reads ---- */
    {
        reply *rr;
        char t[300], first[64], last[64];
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages", carol.uid);
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 200 && strstr(body_of(rr), "\"body\":\"hi\"") && strstr(body_of(rr), "\"outgoing\":false") && strstr(body_of(rr), "\"next\":null"));
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 404);                                                                 /* no one: no conversation */

        /* the whole chat with Alice, in pages of twenty, oldest of each page first */
        size_t total = stored(chat_a);
        int seen = 0;
        size_t offset = 0;
        int64_t prev_ts_of_older_page = 0;
        (void)prev_ts_of_older_page;
        int pages = 0;
        for (;;) {
            snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=20&offset=%zu", alice.uid, offset);
            rr = owner("GET", t, NULL);
            CHECK(rr->status == 200);
            int in_page = count_in(body_of(rr), "\"outgoing\"");
            seen += in_page;
            pages++;
            puddle_span arr, it;
            size_t cur = 0;
            int64_t last_ts = 0;
            const char *bd = body_of(rr);
            CHECK(puddle_json_member(bd, strlen(bd), "items", &arr) == 0);
            while (puddle_json_array_next(arr, &cur, &it) == 1) {
                puddle_span tv;
                int64_t ts = 0;
                CHECK(puddle_json_member(it.p, it.len, "ts", &tv) == 0 && puddle_json_int64(tv, &ts) == 0 && ts > last_ts);   /* chat order */
                last_ts = ts;
            }
            puddle_span nx;
            CHECK(puddle_json_member(bd, strlen(bd), "next", &nx) == 0);
            if (nx.len == 4 && !memcmp(nx.p, "null", 4)) break;
            offset = (size_t)atoi(nx.p);
            if (pages > 30) { failures++; break; }
        }
        CHECK((size_t)seen == total && pages == (int)((total + 19) / 20));
        (void)first; (void)last;
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=1", alice.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 1 && strstr(body_of(rr), "\"outgoing\":true"));  /* the newest is the 4000-character one we sent */
        const char *badq[] = { "?limit=0", "?limit=201", "?limit=x", "?offset=-1", "?x=1", "?user=%s", "?limit=1&limit=2" };
        for (size_t i = 0; i < sizeof badq / sizeof *badq; i++) {
            char q[300];
            snprintf(q, sizeof q, badq[i], alice.uid);
            snprintf(t, sizeof t, "/api/v1/conversations/%s/messages%s", alice.uid, q);
            if (owner("GET", t, NULL)->status != 400) { fprintf(stderr, "FAIL query %s accepted\n", t); failures++; }
        }
        CHECK(owner("GET", "/api/v1/conversations/junk/messages", NULL)->status == 404);
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages", alice.uid);
        CHECK(call("GET", t, NULL, NULL, 0)->status == 404);

        /* long messages: a page holds what fits, and no message is lost between pages */
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);                                                   /* (blocking her closed the stream) */
        char longbody[4001 * 4];
        for (int i = 0; i < 4000; i++) memcpy(longbody + i * 4, "\xF0\x9F\x99\x82", 4);
        longbody[16000] = 0;
        for (int i = 0; i < 8; i++) {
            clock_now += 1000;
            env e = make_env(&carol, &carol, chat_c, T0 + 500000 + i, longbody, NULL);
            char *dl = delivery(chat_c, &e, 1);
            CHECK(strlen(e.event) <= PUDDLE_EVENT_MAX_BYTES);
            r = peer_post(&carol, dl);
            CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\""));
            free(dl); free(e.event);
        }
        int got = 0, rounds = 0;
        offset = 0;
        for (;;) {
            snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=200&offset=%zu", carol.uid, offset);
            rr = owner("GET", t, NULL);
            const char *bd = body_of(rr);
            CHECK(rr->status == 200 && puddle_json_valid_object(bd, strlen(bd)));
            got += count_in(bd, "\"outgoing\"");
            rounds++;
            puddle_span nx;
            CHECK(puddle_json_member(bd, strlen(bd), "next", &nx) == 0);
            if (nx.len == 4 && !memcmp(nx.p, "null", 4)) break;
            offset = (size_t)atoi(nx.p);
            if (rounds > 20) { failures++; break; }
        }
        CHECK(got == 9 && rounds >= 2);
    }

    /* ---- editing and taking back ---- */
    {
        /* a fresh pair of people so the earlier chats stay as they were */
        person dina;
        make(&dina, "dina");
        set_peer(&dina, PUDDLE_STREAM_ACTIVE);
        char chat_d[64], t[600], bodyj[900];
        chat_with_me(&dina, chat_d);
        puddle_ob_item *l;
        size_t n;

        /* messages in both directions */
        reply *rr;
        char mine[3][65];
        for (int i = 0; i < 3; i++) {
            clock_now += 1000;
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine %d\"}", i); dm_to = dina.uid;
            rr = dm_send(dm_to, bodyj);
            CHECK(rr->status == 201);
            const char *p = strstr(body_of(rr), "\"id\":\"");
            snprintf(mine[i], 65, "%.64s", p + 6);
        }
        env theirs = make_env(&dina, &dina, chat_d, T0 + 900000, "theirs 0", NULL);
        char *dl = delivery(chat_d, &theirs, 1);
        clock_now += 1000;
        CHECK(peer_post(&dina, dl)->status == 200);
        free(dl);

        /* edit */
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 1, corrected\"}"); dm_to = dina.uid; dm_msg = mine[1];
        CHECK(call("POST", conv_path(dm_to, "messages/x/edit"), bodyj, NULL, 0)->status == 404);                        /* not the owner */
        rr = dm_edit(dm_to, dm_msg, bodyj);
        CHECK(rr->status == 200 && strstr(body_of(rr), "\"body\":\"mine 1, corrected\"") && strstr(body_of(rr), "\"edited_at\"") &&
              strstr(body_of(rr), "\"edited_at\":") && strstr(body_of(rr), mine[1]) && strstr(body_of(rr), "\"outgoing\":true"));
        puddle_outbox_list(ob, &l, &n);
        {
            int found = 0;
            for (size_t i = 0; i < n; i++)
                if (!strcmp(l[i].peer, dina.uid) && strstr(l[i].body, mine[1]) && strstr(l[i].body, "\\\"type\\\":\\\"edit\\\"")) found++;
            CHECK(found == 1);                                                                        /* the edit is queued as an event of its own */
        }
        puddle_outbox_list_free(l, n);
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 200 && count_in(body_of(rr), "\"outgoing\"") == 4 && strstr(body_of(rr), "mine 1, corrected") && !strstr(body_of(rr), "\"mine 1\""));
        CHECK(count_in(body_of(rr), "\"edited_at\"") == 1);                                         /* edits are not messages: four, not five */
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 1, corrected twice\"}"); dm_to = dina.uid; dm_msg = mine[1];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "corrected twice") && !strstr(body_of(rr), "mine 1, corrected\"") && count_in(body_of(rr), "\"outgoing\"") == 4);   /* the last one stands */

        /* what cannot be edited, and why */
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"x\"}"); dm_to = dina.uid; dm_msg = theirs.id;
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 404);                                /* hers, not ours */
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"x\"}"); dm_to = dina.uid; dm_msg = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 404);
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"\"}"); dm_to = dina.uid; dm_msg = mine[0];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 400);
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"bell\\u0007\"}"); dm_to = dina.uid; dm_msg = mine[0];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 400);
        snprintf(bodyj, sizeof bodyj, "{}"); dm_to = dina.uid; dm_msg = mine[0];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 400);                                /* no text */
        CHECK(dm_edit(dina.uid, "short", "{\"body\":\"x\"}")->status == 404);                       /* not a message id: no such path */
        CHECK(dm_edit("junk", mine[0], "{\"body\":\"x\"}")->status == 404);
        CHECK(dm_edit(dina.uid, mine[0], "nonsense")->status == 400);

        /* taking back */
        clock_now += 1000;
        size_t before = stored(chat_d);
        snprintf(bodyj, sizeof bodyj, "{}"); dm_to = dina.uid; dm_msg = mine[0];
        rr = dm_delete(dm_to, dm_msg);
        CHECK(rr->status == 200 && strstr(body_of(rr), "\"deleted\":true") && !strstr(body_of(rr), "\"body\"") && stored(chat_d) == before + 1);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 4 && count_in(body_of(rr), "\"deleted\":true") == 1 && !strstr(body_of(rr), "\"mine 0\""));
        CHECK(count_in(body_of(rr), "\"body\"") == 3);                                                 /* the gap has no body at all, not an empty one */
        CHECK(strstr(body_of(rr), mine[0]));                                                          /* still there, as a gap, in its place */
        CHECK(dm_delete(dm_to, dm_msg)->status == 200 && stored(chat_d) == before + 1);   /* again: nothing more is sent */
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"too late\"}"); dm_to = dina.uid; dm_msg = mine[0];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 409 && strstr(body_of(dm_edit(dm_to, dm_msg, bodyj)), "deleted"));
        snprintf(bodyj, sizeof bodyj, "{}"); dm_to = dina.uid; dm_msg = theirs.id;
        CHECK(dm_delete(dm_to, dm_msg)->status == 404);                              /* hers */
        snprintf(bodyj, sizeof bodyj, "{}"); dm_to = dina.uid; dm_msg = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        CHECK(dm_delete(dm_to, dm_msg)->status == 404);
        CHECK(dm_delete(dina.uid, "nope")->status == 404);

        /* paging counts messages, however many edits sit between them */
        for (int i = 0; i < 6; i++) {
            clock_now += 1000;
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 2, edit %d\"}", i); dm_to = dina.uid; dm_msg = mine[2];
            CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);
        }
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=1&offset=3", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 1 && strstr(body_of(rr), mine[0]) && strstr(body_of(rr), "\"next\":null"));   /* the oldest of four */
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=2&offset=1", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 2 && strstr(body_of(rr), "mine 1, corrected twice") && strstr(body_of(rr), "mine 2, edit 5") &&
              strstr(body_of(rr), "\"next\":3"));                                                       /* (hers is the newest; then mine 2 and mine 1) */

        /* what has to be said to Dina: no stream, no editing */
        set_peer(&dina, PUDDLE_STREAM_CLOSED);
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"x\"}"); dm_to = dina.uid; dm_msg = mine[2];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 403);
        snprintf(bodyj, sizeof bodyj, "{}"); dm_to = dina.uid; dm_msg = mine[2];
        CHECK(dm_delete(dm_to, dm_msg)->status == 403);
        set_peer(&dina, PUDDLE_STREAM_ACTIVE);

        /* far back: the edits of a message that is many events behind */
        for (int i = 0; i < 70; i++) {
            clock_now += 10;
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 2, many %d\"}", i); dm_to = dina.uid; dm_msg = mine[2];
            CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);
        }
        env late = make_env(&dina, &dina, chat_d, T0 + 900001, "theirs 1", NULL);
        dl = delivery(chat_d, &late, 1);
        clock_now += 1000;
        CHECK(peer_post(&dina, dl)->status == 200);
        free(dl);
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=1&offset=3", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "mine 1, corrected twice") && count_in(body_of(rr), "\"outgoing\"") == 1);   /* behind seventy events */
        snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=1&offset=2", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "mine 2, many 69"));                                                /* 70 edits later, the last of them */
        /* an edit of something even further back, and a clock that has gone backwards */
        for (int i = 0; i < 60; i++) {
            clock_now += 10;
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 2, more %d\"}", i); dm_to = dina.uid; dm_msg = mine[2];
            CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);
        }
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"body\":\"mine 1, found far back\"}"); dm_to = dina.uid; dm_msg = mine[1];
        CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);                                /* more than 128 events behind */
        {
            clock_now += 5000000;                                                                      /* a message stamped well ahead... */
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"stamped ahead\"}"); dm_to = dina.uid;
            rr = dm_send(dm_to, bodyj);
            char ahead[65];
            const char *ip = strstr(body_of(rr), "\"id\":\"");
            snprintf(ahead, sizeof ahead, "%.64s", ip + 6);
            clock_now -= 4000000;                                                                      /* ...and then the clock is set back */
            snprintf(bodyj, sizeof bodyj, "{\"body\":\"fixed anyway\"}"); dm_to = dina.uid; dm_msg = ahead;
            CHECK(dm_edit(dm_to, dm_msg, bodyj)->status == 200);
            snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=200", dina.uid);
            rr = owner("GET", t, NULL);
            CHECK(strstr(body_of(rr), "fixed anyway") && !strstr(body_of(rr), "stamped ahead"));        /* the edit counted: it was stamped after what it edits */
            clock_now += 1000000;
        }
        free(theirs.event); free(late.event);

        /* ---- and from the other side ---- */
        {
            char ev[PUDDLE_EVENT_MAX_BYTES];
            env es[4];
            /* Dina edits and takes back her own, and also tries it on one of ours */
            size_t el = puddle_edit_build(chat_d, dina.uid, T0 + 900100, theirs.id, "theirs 0, edited", 16, ev, sizeof ev);
            es[0] = env_of(&dina, ev);
            el = puddle_edit_build(chat_d, dina.uid, T0 + 900101, mine[2], "she rewrites ours", 17, ev, sizeof ev);
            es[1] = env_of(&dina, ev);
            el = puddle_redact_build(chat_d, dina.uid, T0 + 900102, mine[2], ev, sizeof ev);
            es[2] = env_of(&dina, ev);
            el = puddle_redact_build(chat_d, dina.uid, T0 + 900103, late.id, ev, sizeof ev);
            es[3] = env_of(&dina, ev);
            (void)el;
            dl = delivery(chat_d, es, 4);
            clock_now += 1000;
            rr = peer_post(&dina, dl);
            CHECK(rr->status == 200 && count_in(body_of(rr), "\"reason\"") == 0);
            free(dl);
            for (int i = 0; i < 4; i++) free(es[i].event);
            snprintf(t, sizeof t, "/api/v1/conversations/%s/messages?limit=200", dina.uid);
            rr = owner("GET", t, NULL);
            CHECK(strstr(body_of(rr), "theirs 0, edited") && count_in(body_of(rr), "\"outgoing\":false") == 2);           /* her edit of her own message shows */
            CHECK(!strstr(body_of(rr), "she rewrites ours") && strstr(body_of(rr), "mine 2, more 59"));                      /* her edit of ours does not */
            CHECK(count_in(body_of(rr), "\"deleted\":true") == 2);                                                        /* ours (mine 0) and her own (theirs 1) */
            /* she cannot, though, say it as someone else */
            el = puddle_edit_build(chat_d, me.uid, T0 + 900200, mine[2], "as the owner", 12, ev, sizeof ev);
            env bad = env_of(&dina, ev);
            dl = delivery(chat_d, &bad, 1);
            clock_now += 1000;
            rr = peer_post(&dina, dl);
            CHECK(strstr(body_of(rr), "wrong_sender"));
            free(dl);
            free(bad.event);
        }
        puddle_identity_close(dina.id);
    }

    /* ---- a message that could not be queued says so ---- */
    {
        puddle_outbox_close(ob);
        hooks.max_queued = 1;
        ob = puddle_outbox_open(data, &hooks, err, sizeof err);
        ctx.outbox = ob;
        char body[300];
        snprintf(body, sizeof body, "{\"body\":\"queue is full\"}"); dm_to = alice.uid;
        size_t before = stored(chat_a);
        r = dm_send(dm_to, body);
        CHECK(r->status == 500 && strstr(body_of(r), "not_queued") && stored(chat_a) == before + 1);   /* kept, and honest about it */
        ctx.outbox = NULL;
        r = dm_send(dm_to, body);
        CHECK(r->status == 500 && strstr(body_of(r), "not_queued"));
    }

    ctx.outbox = ob;

    /* ---- conversations and what has been read ---- */
    {
        person dave;
        make(&dave, "dave");
        clock_now += 100000000; /* well after everything above, so this is the newest conversation */
        char data2[720];
        snprintf(data2, sizeof data2, "%s/data2", root);
        puddle_outbox_hooks hooks2 = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb, .addresses = addr_cb };
        puddle_outbox *ob2 = puddle_outbox_open(data2, &hooks2, err, sizeof err); /* the first one was filled by the tests above */
        CHECK(ob2 != NULL);
        ctx.outbox = ob2;
        set_peer(&dave, PUDDLE_STREAM_ACTIVE);
        char chat_d[64];
        chat_with_me(&dave, chat_d);
        CHECK(read_at(dave.uid) == 0);
        clock_now += 100000;
        env d1 = make_env(&dave, &dave, chat_d, clock_now, "first\nline", NULL);
        char *db = delivery(chat_d, &d1, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);
        clock_now += 1000;
        env d2 = make_env(&dave, &dave, chat_d, clock_now, "second", NULL);
        db = delivery(chat_d, &d2, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);

        r = owner("GET", "/api/v1/conversations", NULL);
        b = (char *)body_of(r);
        CHECK(r->status == 200 && strstr(b, "\"items\":[{\"id\":\"") && strstr(b, "\"kind\":\"dm\""));
        const char *mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"preview\":\"second\"") && strstr(mine, "\"unread\":2") && strstr(mine, "\"outgoing\":false"));
        CHECK(strstr(b, dave.uid) == strstr(b, "\"items\":[{\"id\":\"") + 16); /* the newest conversation comes first */
        CHECK(mine && strstr(mine, "\"recipient\":{\"id\":\"") && strstr(mine, "\"stream\":\"active\""));
        r = owner("GET", conv_path(dave.uid, NULL), NULL);                         /* one on its own */
        CHECK(r->status == 200 && strstr(body_of(r), "\"preview\":\"second\"") && strstr(body_of(r), "\"unread\":2"));
        CHECK(owner("GET", conv_path("zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL), NULL)->status == 404);
        CHECK(owner("GET", "/api/v1/conversations?limit=0", NULL)->status == 400);
        CHECK(owner("GET", "/api/v1/conversations?offset=x", NULL)->status == 400);
        CHECK(call("GET", "/api/v1/conversations", NULL, NULL, 0)->status == 404);

        const char *read_path = conv_path(dave.uid, "read");
        r = owner("POST", read_path, NULL);
        CHECK(r->status == 200 && strstr(body_of(r), "\"unread\":0"));
        CHECK(read_at(dave.uid) == clock_now);
        r = owner("GET", "/api/v1/conversations", NULL);
        mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"unread\":0"));
        CHECK(owner("POST", "/api/v1/conversations/nope/read", NULL)->status == 404);
        CHECK(call("POST", conv_path(dave.uid, "read"), NULL, NULL, 0)->status == 404);

        /* a later message is unread again; the marker never goes backwards */
        clock_now += 1000;
        env d3 = make_env(&dave, &dave, chat_d, clock_now, "third", NULL);
        db = delivery(chat_d, &d3, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);
        r = owner("GET", "/api/v1/conversations", NULL);
        mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"unread\":1"));
        int64_t m = read_at(dave.uid);
        CHECK(puddle_conversations_set_read(cs, dave.uid, chat_d, PUDDLE_CONV_DM, 5) == 0 && read_at(dave.uid) == m);

        /* it survives a restart */
        puddle_streams_close(ss);
        ss = puddle_streams_open(data, err, sizeof err);
        ctx.streams = ss;
        ctx.peer_key_user = ss;
        puddle_store_on_append(store, NULL, NULL);
        puddle_conversations_close(cs);
        cs = puddle_conversations_open(data, err, sizeof err);
        puddle_store_on_append(store, puddle_conversations_appended, cs);
        ctx.conversations = cs;
        CHECK(read_at(dave.uid) == m);
        r = owner("GET", "/api/v1/conversations", NULL); /* worked out again from the log, with the same marker */
        mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"unread\":1"));

        /* the owner's own message shows as outgoing, and a deleted one has no preview */
        {
            char sb[300];
            snprintf(sb, sizeof sb, "{\"body\":\"mine\"}"); dm_to = dave.uid;
            clock_now += 1000;
            CHECK(dm_send(dm_to, sb)->status == 201);
            r = owner("GET", "/api/v1/conversations", NULL);
            mine = conv_of(r, dave.uid);
            CHECK(mine && strstr(mine, "\"preview\":\"mine\"") && strstr(mine, "\"outgoing\":true") && strstr(mine, "\"unread\":1"));
        }
        /* what became of what we sent: waiting, retrying, failed, and a way to try again */
        {
            const char *mk = "/api/v1/conversations/%s/messages";
            char url[300], sb2[300];
            snprintf(url, sizeof url, mk, dave.uid);
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            const char *last = strrchr(b, '{');                                     /* the newest message, "mine" */
            CHECK(strstr(b, "\"body\":\"mine\"") && strstr(strstr(b, "\"body\":\"mine\""), "\"delivery\":{\"state\":\"queued\""));
            CHECK(!strstr(b, "\"body\":\"third\",\"outgoing\":false,\"delivery\"") && last);
            CHECK(count_in(b, "\"delivery\"") == 1);                                /* theirs have none */

            g_addr_mode = 0;
            puddle_outbox_poll(ob2);                                                /* no way to reach them: fails once, with the reason */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            CHECK(strstr(b, "\"delivery\":{\"state\":\"failed\",\"reason\":\"we have no address for them\""));
            CHECK(owner("POST", conv_path(dave.uid, "retry"), NULL)->status == 200);   /* again */
            r = owner("GET", url, NULL);
            CHECK(strstr(body_of(r), "\"state\":\"queued\"") && !strstr(body_of(r), "\"state\":\"failed\""));

            g_addr_mode = 1;
            puddle_outbox_poll(ob2);                                                /* they are offline: it will be tried again */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            CHECK(strstr(b, "\"delivery\":{\"state\":\"retrying\"") && strstr(b, "\"next_at\":") && strstr(b, "\"attempts\":1"));
            CHECK(owner("POST", conv_path(dave.uid, "retry"), NULL)->status == 200);   /* try now */

            /* an edit that is waiting is not mistaken for the delivery of the message it edits */
            char sbm[300];
            snprintf(sbm, sizeof sbm, "{\"body\":\"mine2\"}"); dm_to = dave.uid;
            clock_now += 1000;
            CHECK(dm_send(dm_to, sbm)->status == 201);
            g_addr_mode = 0;
            puddle_outbox_poll(ob2);                                                /* "mine2" fails */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            {
                const char *at = strstr(b, "\"body\":\"mine2\"");
                CHECK(at != NULL);
                char mid[PUDDLE_ID_HEX + 1] = "";
                for (const char *q = at; q > b; q--)
                    if (!strncmp(q, "{\"id\":\"", 7) && !strncmp(q + 7 + PUDDLE_ID_HEX, "\",\"conversation\"", 16)) { /* the message's, not its author's */
                        memcpy(mid, q + 7, PUDDLE_ID_HEX);
                        mid[PUDDLE_ID_HEX] = 0;
                        break;
                    }
                CHECK(strlen(mid) == PUDDLE_ID_HEX);
                puddle_ob_item *l;
                size_t cnt;
                CHECK(puddle_outbox_list(ob2, &l, &cnt) == 0);
                for (size_t i = 0; i < cnt; i++)
                    if (!strcmp(l[i].peer, dave.uid) && strstr(l[i].body, mid) && l[i].state == PUDDLE_OB_FAILED)
                        CHECK(puddle_outbox_dismiss(ob2, l[i].seq) == 0);          /* delivered, as far as the outbox can tell */
                puddle_outbox_list_free(l, cnt);
                char eb[400];
                snprintf(eb, sizeof eb, "{\"body\":\"mine2 edited\"}"); dm_to = dave.uid; dm_msg = mid;
                clock_now += 1000;
                CHECK(dm_edit(dm_to, dm_msg, eb)->status == 200);
                r = owner("GET", url, NULL);
                b = (char *)body_of(r);
                CHECK(strstr(b, "\"body\":\"mine2 edited\"") && count_in(b, "\"delivery\"") == 1); /* only "mine" still has one */
            }
            (void)sb2;
            r = owner("POST", conv_path(alice.uid, "retry"), NULL);
            CHECK(r->status == 404 && strstr(body_of(r), "nothing_to_retry"));      /* nothing to retry for someone else */
            CHECK(owner("POST", "/api/v1/conversations/nope/retry", NULL)->status == 404);
            CHECK(call("POST", conv_path(dave.uid, "retry"), NULL, NULL, 0)->status == 404);
        }
        free(d1.event); free(d2.event); free(d3.event);
        ctx.outbox = ob;
        puddle_outbox_close(ob2);
    }

    puddle_outbox_close(ob);
    puddle_streams_close(ss);
    puddle_store_close(store);
    puddle_conversations_close(cs);
    puddle_replay_free(ctx.replay);
    puddle_identity_close(me.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

    return failures ? 1 : 0;
}
