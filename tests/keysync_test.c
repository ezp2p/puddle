#include "federation/keysync.h"

#include "federation/flow.h"
#include "base/hash.h"
#include "events/message.h"
#include "core/phrase.h"
#include "service/routes.h"
#include "federation/courier.h"
#include "federation/sender.h"
#include "http/client.h"
#include "platform/fsutil.h"

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
static puddle_outbox *ob;
static puddle_config cfg;

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

static char *chain_of(person *p)
{
    size_t n;
    return puddle_identity_chain(p->id, &n);
}

static void set_peer(person *p, puddle_stream_state state, int64_t chain_sent, int64_t addr_sent)
{
    puddle_stream rec;
    char err[300];
    char *chain = chain_of(p);
    CHECK(puddle_stream_init(&rec, p->uid, state, T0) == 0);
    CHECK(puddle_stream_set_chain(&rec, chain, strlen(chain)) == 0);
    free(chain);
    CHECK(puddle_stream_add_address(&rec, "203.0.113.9:8443") == 0);
    rec.chain_sent = chain_sent;
    rec.addr_sent = addr_sent;
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

typedef struct { int status; char out[8000]; size_t n; } reply;

static reply post_as(person *from, const char *path, const char *body, int sign)
{
    reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "POST");
    snprintf(req.path, sizeof req.path, "%s", path);
    snprintf(req.target, sizeof req.target, "%s", path);
    if (sign && puddle_reqsig_sign(from->id, "POST", path, body, strlen(body), me.uid, clock_now, req.signature, sizeof req.signature) != 0) exit(2);
    size_t bl = strlen(body);
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    memcpy(copy, body, bl + 1);
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, 0, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    clock_now += 10;
    return rp;
}

static char *chain_body(const char *chain)
{
    size_t cap = strlen(chain) * 6 + 64;
    char *b = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, b, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    return b;
}

static int chain_is(person *p, const char *want)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, p->uid, &rec) != 0) return 0;
    int ok = rec.chain && !strcmp(rec.chain, want);
    puddle_stream_free(&rec);
    return ok;
}

static int key_ok(person *p)
{
    unsigned char pub[32], got[32];
    char kid[65];
    puddle_identity_public(p->id, pub, NULL, NULL);
    puddle_key_id(pub, kid);
    return puddle_streams_peer_key(ss, p->uid, kid, got) == 1 && !memcmp(got, pub, 32);
}

/* an address record, signed by `signer`, with `fields` after the id */
static char *address_body(person *signer, const char *as_id, const char *type, const char *addrs, const char *extra, int64_t seq, int64_t ts)
{
    char rec[800], sig[PUDDLE_SIG_TEXT_MAX];
    snprintf(rec, sizeof rec, "{\"type\":\"%s\",\"id\":\"%s\",\"addresses\":%s%s%s,\"seq\":%lld,\"ts\":%lld}", type, as_id, addrs,
             extra && *extra ? "," : "", extra ? extra : "", (long long)seq, (long long)ts);
    if (puddle_text_sign(signer->id, PUDDLE_ADDRESS_DOMAIN, rec, strlen(rec), sig) != 0) exit(2);
    char *b = malloc(2000);
    puddle_jw w;
    puddle_jw_init(&w, b, 2000);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "record", rec);
    puddle_jw_str(&w, "sig", sig);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    return b;
}

static int queued(const char *peer, const char *kind)
{
    puddle_ob_item *l;
    size_t n;
    int c = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        c += !strcmp(l[i].peer, peer) && (!kind || !strcmp(l[i].kind, kind)) && l[i].state != PUDDLE_OB_FAILED;
    puddle_outbox_list_free(l, n);
    return c;
}

static char *item_body(const char *peer, const char *kind, uint64_t *seq)
{
    puddle_ob_item *l;
    size_t n;
    char *body = NULL;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(l[i].peer, peer) && !strcmp(l[i].kind, kind)) {
            free(body);
            body = strdup(l[i].body);
            if (seq) *seq = l[i].seq;
        }
    puddle_outbox_list_free(l, n);
    return body;
}

static void clear_outbox(void)
{
    puddle_outbox_close(ob);
    char path[900], err[200];
    snprintf(path, sizeof path, "%s/state/outbox", data);
    nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    puddle_outbox_hooks h = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &h, err, sizeof err);
    ctx.outbox = ob;
}

static void write_outbox_item(unsigned long long seq, const char *peer, const char *kind, const char *path, const char *body, const char *state)
{
    char file[900], esc[2000], text[4000];
    size_t j = 0;
    for (const char *c = body; *c; c++) {
        if (*c == '"' || *c == '\\') esc[j++] = '\\';
        esc[j++] = *c;
    }
    esc[j] = 0;
    snprintf(text, sizeof text, "{\"v\":1,\"seq\":%llu,\"peer\":\"%s\",\"kind\":\"%s\",\"path\":\"%s\",\"body\":\"%s\",\"created\":%lld,\"next\":%lld,"
             "\"first_offline\":0,\"attempts\":1,\"state\":\"%s\",\"status\":0,\"reason\":\"x\"}", seq, peer, kind, path, esc, (long long)T0, (long long)T0, state);
    snprintf(file, sizeof file, "%s/state/outbox", data);
    puddle_mkdir_p(file); /* the folder is made with the first item the outbox keeps itself */
    snprintf(file, sizeof file, "%s/state/outbox/%012llu.json", data, seq);
    FILE *f = fopen(file, "wb");
    fputs(text, f);
    fclose(f);
}

static void reopen_outbox(void)
{
    puddle_outbox_close(ob);
    char err[200];
    puddle_outbox_hooks h = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &h, err, sizeof err);
    ctx.outbox = ob;
}

static int64_t sent_of(person *p, int addr)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, p->uid, &rec) != 0) return -99;
    int64_t v = addr ? rec.addr_sent : rec.chain_sent;
    puddle_stream_free(&rec);
    return v;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-keysync-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&carol, "carol");
    char err[300];
    snprintf(data, sizeof data, "%s/data", root);
    mkdir(root, 0700);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", data);
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    ss = puddle_streams_open(data, err, sizeof err);
    puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .identity = me.id, .streams = ss, .outbox = ob, .replay = puddle_replay_new(4096), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    snprintf(ctx.own_tls_fp, sizeof ctx.own_tls_fp, "%s", "1111111111111111111111111111111111111111111111111111111111111111");
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply r;
    char *b, *v0, *v1, *v2;

    /* ---- they changed keys ---- */
    set_peer(&alice, PUDDLE_STREAM_ACTIVE, 0, 0);
    v0 = chain_of(&alice);
    CHECK(key_ok(&alice));
    roll(&alice);
    v1 = chain_of(&alice);
    CHECK(!key_ok(&alice));                                                                     /* her new key is not one we hold */
    b = chain_body(v1);
    r = post_as(&alice, "/stream/v1/identity", b, 1);
    CHECK(r.status == 202 && strstr(r.out, "Content-Length: 0\r\n") && chain_is(&alice, v1) && key_ok(&alice));
    r = post_as(&alice, "/stream/v1/identity", b, 1);
    CHECK(r.status == 202 && chain_is(&alice, v1));                                              /* again: nothing changes */
    free(b);
    b = chain_body(v0);                                                                          /* the old chain, signed with her old key... */
    {
        char sig[800];
        /* ...which she can no longer sign with: signed with the new key, naming the old chain's key, it fails to authenticate */
        puddle_reqsig_sign(alice.id, "POST", "/stream/v1/identity", b, strlen(b), me.uid, clock_now, sig, sizeof sig);
        puddle_request req;
        memset(&req, 0, sizeof req);
        strcpy(req.method, "POST"); strcpy(req.path, "/stream/v1/identity"); strcpy(req.target, "/stream/v1/identity");
        snprintf(req.signature, sizeof req.signature, "%s", sig);
        req.content_length = strlen(b);
        char out[4000]; int st;
        puddle_router_handle(&R, &req, 0, (unsigned char *)b, strlen(b), out, sizeof out, &st);
        CHECK(st == 404 && chain_is(&alice, v1));
    }
    free(b);
    roll(&alice);
    v2 = chain_of(&alice);
    b = chain_body(v2);
    CHECK(post_as(&alice, "/stream/v1/identity", b, 1).status == 202 && chain_is(&alice, v2));
    free(b);
    b = chain_body(v1);                                                                          /* an older chain than we hold: not news */
    CHECK(post_as(&alice, "/stream/v1/identity", b, 1).status == 404);                           /* (signed by v2's key, v1 does not name it) */
    free(b);

    /* a fork: someone who got hold of an old phrase makes a different next record from the same point. It is a valid chain for
     * her id, and signed by its own head key, but it is not a continuation of the chain we hold, so it is taken for nothing. */
    {
        person fork;
        char dir[900], cmd[2000], phrase_at_v0[PUDDLE_PHRASE_LEN + 1];
        (void)phrase_at_v0;
        /* a copy of a fresh identity before it rolls, and the real one rolling, and the copy rolling differently */
        person orig;
        make(&orig, "orig");
        set_peer(&orig, PUDDLE_STREAM_ACTIVE, 0, 0);
        snprintf(dir, sizeof dir, "%s/orig-copy", root);
        snprintf(cmd, sizeof cmd, "cp -R '%s/orig' '%s'", root, dir);
        CHECK(system(cmd) == 0);
        fork.id = puddle_identity_open(dir, err, sizeof err);
        CHECK(fork.id != NULL);
        memcpy(fork.phrase, orig.phrase, sizeof fork.phrase);
        puddle_identity_id(fork.id, fork.uid);
        CHECK(!strcmp(fork.uid, orig.uid));
        roll(&orig);
        char *real = chain_of(&orig);
        b = chain_body(real);
        CHECK(post_as(&orig, "/stream/v1/identity", b, 1).status == 202 && chain_is(&orig, real));
        free(b);
        roll(&fork);                                                                             /* a different record 1 */
        char *f1 = chain_of(&fork);
        CHECK(strcmp(f1, real) != 0 && strlen(f1) > 10);
        b = chain_body(f1);
        CHECK(post_as(&fork, "/stream/v1/identity", b, 1).status == 202 && chain_is(&orig, real) && !chain_is(&orig, f1));   /* same length, different */
        free(b);
        roll(&fork);
        char *f2 = chain_of(&fork);
        b = chain_body(f2);
        CHECK(post_as(&fork, "/stream/v1/identity", b, 1).status == 202 && chain_is(&orig, real) && !chain_is(&orig, f2));   /* longer, but not ours */
        free(b);
        CHECK(key_ok(&orig));                                                                    /* and her key is still the real one */
        free(real); free(f1); free(f2);
        puddle_identity_close(fork.id);
        puddle_identity_close(orig.id);
    }

    /* a stranger with chain of their own; someone else's chain under her id; no chain; junk */
    {
        char *cc = chain_of(&carol);
        b = chain_body(cc);
        r = post_as(&carol, "/stream/v1/identity", b, 1);
        CHECK(r.status == 202);
        puddle_stream rec;
        CHECK(puddle_streams_get(ss, carol.uid, &rec) == 1);                                      /* nothing is made of a stranger */
        free(b);
        CHECK(post_as(&alice, "/stream/v1/identity", "{}", 1).status == 404);
        CHECK(post_as(&alice, "/stream/v1/identity", "{\"chain\":5}", 1).status == 404);
        CHECK(post_as(&alice, "/stream/v1/identity", "{\"chain\":\"\"}", 1).status == 404);
        CHECK(post_as(&alice, "/stream/v1/identity", "{\"chain\":\"junk\"}", 1).status == 404);
        b = chain_body(cc);
        CHECK(post_as(&alice, "/stream/v1/identity", b, 1).status == 404);                        /* Carol's chain, Alice's signature */
        CHECK(post_as(&alice, "/stream/v1/identity", b, 0).status == 404);                        /* unsigned */
        free(b);
        free(cc);
        /* someone we hold nothing of the chain for, who is waiting (they asked): their new chain is accepted too */
        set_peer(&carol, PUDDLE_STREAM_PENDING_IN, -1, -1);
        char *c0 = chain_of(&carol);
        roll(&carol);
        char *c1 = chain_of(&carol);
        b = chain_body(c1);
        CHECK(post_as(&carol, "/stream/v1/identity", b, 1).status == 202 && chain_is(&carol, c1) && !chain_is(&carol, c0));
        free(b); free(c0);
        /* blocked: nothing is taken */
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        roll(&carol);
        char *c2 = chain_of(&carol);
        b = chain_body(c2);
        CHECK(post_as(&carol, "/stream/v1/identity", b, 1).status == 202 && chain_is(&carol, c1));
        free(b); free(c1); free(c2);
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
    }

    /* ---- they moved ---- */
    {
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, 0, 0);                                           /* starts again with v2 and addr_seq 0 */
        const char *fp = "2222222222222222222222222222222222222222222222222222222222222222";
        char extra[200];
        snprintf(extra, sizeof extra, "\"tls_fp\":\"%s\"", fp);
        puddle_stream rec;
        /* a certificate we had noted as different, and a note that something is held */
        CHECK(puddle_streams_check_fingerprint(ss, alice.uid, "3333333333333333333333333333333333333333333333333333333333333333") == 1);
        CHECK(puddle_streams_check_fingerprint(ss, alice.uid, fp) == 0);                         /* now it is "seen", waiting for the owner */
        b = address_body(&alice, alice.uid, "address", "[\"198.51.100.4:9000\",\"[2001:db8::7]:9000\"]", extra, 1, T0 + 500);
        r = post_as(&alice, "/stream/v1/address", b, 1);
        CHECK(r.status == 204 && !strstr(r.out, "Content-Length"));
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && rec.addr_seq == 1 && rec.naddr == 2 && !strcmp(rec.addresses[0], "198.51.100.4:9000") &&
              !strcmp(rec.addresses[1], "[2001:db8::7]:9000") && !strcmp(rec.tls_fp, fp) && !rec.tls_fp_seen[0]);   /* the new certificate is accepted quietly */
        puddle_stream_free(&rec);
        free(b);
        /* the highest seq wins: the same, or an older one, changes nothing */
        b = address_body(&alice, alice.uid, "address", "[\"192.0.2.1:1\"]", NULL, 1, T0 + 600);
        CHECK(post_as(&alice, "/stream/v1/address", b, 1).status == 204);
        free(b);
        b = address_body(&alice, alice.uid, "address", "[\"192.0.2.1:1\"]", NULL, 0, T0 + 600);
        CHECK(post_as(&alice, "/stream/v1/address", b, 1).status == 204);
        free(b);
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && !strcmp(rec.addresses[0], "198.51.100.4:9000") && rec.addr_seq == 1);
        puddle_stream_free(&rec);
        /* newer, with no certificate and no addresses: the address list and certificate stay */
        b = address_body(&alice, alice.uid, "address", "[]", NULL, 2, T0 + 700);
        CHECK(post_as(&alice, "/stream/v1/address", b, 1).status == 204);
        free(b);
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && rec.addr_seq == 2 && rec.naddr == 2 && !strcmp(rec.tls_fp, fp));
        puddle_stream_free(&rec);
        /* what is refused */
        struct { const char *type, *id_is, *addrs, *extra; int64_t seq, ts; person *signer; } bad[] = {
            { "other", NULL, "[]", NULL, 5, T0, &alice },                                         /* not an address record */
            { "address", "carol", "[]", NULL, 5, T0, &alice },                                    /* about someone else */
            { "address", NULL, "[\"not an address\"]", NULL, 5, T0, &alice },
            { "address", NULL, "[\"1.2.3.4:1\",\"1.2.3.5:1\",\"1.2.3.6:1\",\"1.2.3.7:1\",\"1.2.3.8:1\"]", NULL, 5, T0, &alice },  /* too many */
            { "address", NULL, "[\"0.0.0.0:8443\"]", NULL, 5, T0, &alice },                       /* somewhere we would never connect */
            { "address", NULL, "[]", "\"tls_fp\":\"nothex\"", 5, T0, &alice },
            { "address", NULL, "[]", NULL, -1, T0, &alice },
            { "address", NULL, "[]", NULL, 5, 0, &alice },
            { "address", NULL, "[]", NULL, 5, T0, &carol },                                       /* signed by someone else */
            { "address", NULL, "{}", NULL, 5, T0, &alice },
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            b = address_body(bad[i].signer, bad[i].id_is ? carol.uid : alice.uid, bad[i].type, bad[i].addrs, bad[i].extra, bad[i].seq, bad[i].ts);
            r = post_as(&alice, "/stream/v1/address", b, 1);
            int expect = 400;
            if (r.status != expect) { fprintf(stderr, "FAIL address variant %zu answered %d\n", i, r.status); failures++; }
            free(b);
        }
        CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && rec.addr_seq == 2 && !strcmp(rec.addresses[0], "198.51.100.4:9000"));   /* none of those changed anything */
        puddle_stream_free(&rec);
        {
            /* the name of the key and the signature are each checked: right key id with another's signature, and the reverse */
            char *good = address_body(&alice, alice.uid, "address", "[\"192.0.2.9:9\"]", NULL, 7, T0 + 800);
            char *other = address_body(&alice, alice.uid, "address", "[\"192.0.2.10:9\"]", NULL, 7, T0 + 800);   /* a signature for different bytes */
            puddle_span gs, os;
            char gsig[PUDDLE_SIG_TEXT_MAX + 4], osig[PUDDLE_SIG_TEXT_MAX + 4], ckid[65];
            unsigned char cp[32];
            CHECK(puddle_json_member(good, strlen(good), "sig", &gs) == 0 && puddle_json_string(gs, gsig, sizeof gsig) == 0);
            CHECK(puddle_json_member(other, strlen(other), "sig", &os) == 0 && puddle_json_string(os, osig, sizeof osig) == 0);
            puddle_identity_public(carol.id, cp, NULL, NULL);
            puddle_key_id(cp, ckid);
            char wrong_bytes[PUDDLE_SIG_TEXT_MAX + 4], wrong_kid[PUDDLE_SIG_TEXT_MAX + 4];
            snprintf(wrong_bytes, sizeof wrong_bytes, "%.64s:%s", gsig, strchr(osig, ':') + 1);       /* her key id, a signature for other bytes */
            snprintf(wrong_kid, sizeof wrong_kid, "%s%s", ckid, strchr(gsig, ':'));                   /* a good signature under another key's name */
            char fixed[2400];
            puddle_span rs;
            char rec[900];
            CHECK(puddle_json_member(good, strlen(good), "record", &rs) == 0 && puddle_json_string(rs, rec, sizeof rec) == 0);
            puddle_jw w;
            puddle_jw_init(&w, fixed, sizeof fixed);
            puddle_jw_object(&w);
            puddle_jw_str(&w, "record", rec);
            puddle_jw_str(&w, "sig", wrong_bytes);
            puddle_jw_end_object(&w);
            puddle_jw_finish(&w);
            CHECK(post_as(&alice, "/stream/v1/address", fixed, 1).status == 400);
            puddle_jw_init(&w, fixed, sizeof fixed);
            puddle_jw_object(&w);
            puddle_jw_str(&w, "record", rec);
            puddle_jw_str(&w, "sig", wrong_kid);
            puddle_jw_end_object(&w);
            puddle_jw_finish(&w);
            CHECK(post_as(&alice, "/stream/v1/address", fixed, 1).status == 400);
            CHECK(post_as(&alice, "/stream/v1/address", good, 1).status == 204);                   /* and the real thing is taken */
            free(good); free(other);
        }
        CHECK(post_as(&alice, "/stream/v1/address", "{}", 1).status == 400);
        CHECK(post_as(&alice, "/stream/v1/address", "{\"record\":5,\"sig\":\"x\"}", 1).status == 400);
        CHECK(post_as(&alice, "/stream/v1/address", "{\"record\":\"{}\",\"sig\":\"x\"}", 1).status == 400);
        /* who may say it: a stranger, an unsigned request, a blocked one */
        b = address_body(&carol, carol.uid, "address", "[]", NULL, 9, T0);
        CHECK(post_as(&carol, "/stream/v1/address", b, 0).status == 404);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        CHECK(post_as(&carol, "/stream/v1/address", b, 1).status == 404);
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        free(b);
        /* a record signed by an old key of hers is no good, even in a request signed by the new one */
        {
            char *old_body = address_body(&alice, alice.uid, "address", "[\"192.0.2.2:2\"]", NULL, 20, T0 + 900);
            roll(&alice);
            char *cn = chain_of(&alice);
            char *cb = chain_body(cn);
            CHECK(post_as(&alice, "/stream/v1/identity", cb, 1).status == 202);
            free(cb);
            char *nb = address_body(&alice, alice.uid, "address", "[\"192.0.2.3:3\"]", NULL, 20, T0 + 900);
            CHECK(post_as(&alice, "/stream/v1/address", nb, 1).status == 204);                   /* signed by the key she has now */
            free(nb);
            /* the old body, as she could send before she rolled, signed by the key she had then */
            roll(&alice);
            char *cn2 = chain_of(&alice);
            char *cb2 = chain_body(cn2);
            CHECK(post_as(&alice, "/stream/v1/identity", cb2, 1).status == 202);
            free(cb2); free(cn2); free(cn);
            CHECK(post_as(&alice, "/stream/v1/address", old_body, 1).status == 400);
            free(old_body);
        }
    }
    free(v0); free(v1); free(v2);

    /* ---- telling them: after we change keys ---- */
    {
        clear_outbox();
        puddle_streams_close(ss);
        char path[900];
        snprintf(path, sizeof path, "%s/state/streams", data);
        nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        ss = puddle_streams_open(data, err, sizeof err);
        ctx.streams = ss; ctx.peer_key_user = ss;
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, 0, 0);
        set_peer(&carol, PUDDLE_STREAM_PENDING_OUT, 0, -1);
        person dave, erin;
        make(&dave, "dave");
        make(&erin, "erin");
        set_peer(&dave, PUDDLE_STREAM_CLOSED, 0, 0);
        set_peer(&erin, PUDDLE_STREAM_PENDING_IN, -1, -1);
        CHECK(puddle_sync_resume(&ctx) >= 0);                                                    /* first run: the record starts at 0, nothing to say */
        CHECK(queued(alice.uid, NULL) == 0 && queued(carol.uid, "address") == 0);
        CHECK(queued(alice.uid, "identity") == 0);                                               /* everyone has our chain as it is */

        /* things already waiting, made with the keys we are about to lose */
        {
            char *old_chain = chain_of(&me);
            char pbody[4000];
            snprintf(pbody, sizeof pbody, "{\"requester\":{\"id\":\"%s\",\"address\":\"127.0.0.1:8443\",\"profile\":{\"display_name\":\"Me\"},\"chain\":", me.uid);
            puddle_jw w;
            char tmp[3000];
            puddle_jw_init(&w, tmp, sizeof tmp);
            puddle_jw_string(&w, old_chain);
            size_t tl = puddle_jw_finish(&w);
            strcat(pbody, tmp);
            strcat(pbody, "},\"message\":\"hello\"}");
            CHECK(tl > 0 && puddle_outbox_enqueue(ob, carol.uid, "access", "/stream/v1/access", pbody, strlen(pbody), err, sizeof err) == 0);
            char rbody[4000];
            snprintf(rbody, sizeof rbody, "{\"responder\":{\"id\":\"%s\",\"address\":\"127.0.0.1:8443\",\"profile\":{\"display_name\":\"Me\"},\"chain\":%s},\"approved\":true}", me.uid, tmp);
            CHECK(puddle_outbox_enqueue(ob, alice.uid, "result", "/stream/v1/access/result", rbody, strlen(rbody), err, sizeof err) == 0);
            free(old_chain);

            /* two messages, signed by us as we are now, one already queued, in one delivery */
            char chat[64], ev1[600], ev2[600], s1[PUDDLE_SIG_TEXT_MAX], s2[PUDDLE_SIG_TEXT_MAX], i1[65], i2[65];
            puddle_dm_chat_id(me.uid, alice.uid, chat);
            size_t l1 = puddle_message_build(chat, me.uid, T0 + 1, "one", 3, NULL, ev1, sizeof ev1);
            size_t l2 = puddle_message_build(chat, me.uid, T0 + 2, "two", 3, NULL, ev2, sizeof ev2);
            puddle_event_sign(me.id, ev1, l1, s1);
            puddle_event_sign(me.id, ev2, l2, s2);
            puddle_sha256_hex(ev1, l1, i1);
            puddle_sha256_hex(ev2, l2, i2);
            char dbody[3000];
            puddle_jw d;
            puddle_jw_init(&d, dbody, sizeof dbody);
            puddle_jw_object(&d);
            puddle_jw_str(&d, "chat", chat);
            puddle_jw_key(&d, "events");
            puddle_jw_array(&d);
            puddle_jw_object(&d); puddle_jw_str(&d, "id", i1); puddle_jw_str(&d, "event", ev1); puddle_jw_str(&d, "sig", s1); puddle_jw_end_object(&d);
            puddle_jw_object(&d); puddle_jw_str(&d, "id", i2); puddle_jw_str(&d, "event", ev2); puddle_jw_str(&d, "sig", s2); puddle_jw_end_object(&d);
            puddle_jw_end_array(&d);
            puddle_jw_end_object(&d);
            size_t dl = puddle_jw_finish(&d);
            CHECK(dl && puddle_outbox_enqueue(ob, alice.uid, "events", "/stream/v1/events", dbody, dl, err, sizeof err) == 0);

            roll(&me);                                                                           /* we change keys */
            int n = puddle_sync_after_roll(&ctx);
            CHECK(n >= 4);
            char *new_chain = chain_of(&me);
            /* each peer who holds our chain is told, and no one else */
            CHECK(queued(alice.uid, "identity") == 1 && queued(carol.uid, "identity") == 1);
            CHECK(queued(dave.uid, "identity") == 0 && queued(erin.uid, "identity") == 0);       /* closed, or only asked of us */
            CHECK(queued(alice.uid, "address") == 0);                                            /* nothing about where we are changed */
            char *ib = item_body(alice.uid, "identity", NULL);
            puddle_span cv;
            char *got = ib ? malloc(strlen(ib) + 1) : NULL;
            CHECK(ib && puddle_json_member(ib, strlen(ib), "chain", &cv) == 0 && puddle_json_string(cv, got, strlen(ib) + 1) == 0 && !strcmp(got, new_chain));
            free(ib); free(got);
            /* what was waiting now carries the new chain, and the messages the new signature */
            char *ab = item_body(carol.uid, "access", NULL);
            CHECK(ab && strstr(ab, "\"message\":\"hello\"") && strstr(ab, "\"id\":\"") && strstr(ab, "\"display_name\":\"Me\""));
            puddle_span req;
            CHECK(puddle_json_member(ab, strlen(ab), "requester", &req) == 0 && puddle_json_member(req.p, req.len, "chain", &cv) == 0);
            got = malloc(cv.len + 1);
            CHECK(puddle_json_string(cv, got, cv.len + 1) == 0 && !strcmp(got, new_chain));
            free(got); free(ab);
            char *rb = item_body(alice.uid, "result", NULL);
            CHECK(rb && strstr(rb, "\"approved\":true") && puddle_json_member(rb, strlen(rb), "responder", &req) == 0 && puddle_json_member(req.p, req.len, "chain", &cv) == 0);
            got = malloc(cv.len + 1);
            CHECK(puddle_json_string(cv, got, cv.len + 1) == 0 && !strcmp(got, new_chain));
            free(got); free(rb);
            char *eb = item_body(alice.uid, "events", NULL);
            {
                puddle_span evs = { "", 0 }, ev;
                size_t cur = 0;
                int seen = 0;
                CHECK(eb && puddle_json_member(eb, strlen(eb), "events", &evs) == 0);
                unsigned char pub[32];
                puddle_identity_public(me.id, pub, NULL, NULL);
                char mykid[65];
                puddle_key_id(pub, mykid);
                while (puddle_json_array_next(evs, &cur, &ev) == 1) {
                    puddle_span iv, tv, sv;
                    char idt[80], sigt[PUDDLE_SIG_TEXT_MAX + 4], kid[65], hash[65];
                    unsigned char sg[64];
                    char *evt;
                    CHECK(puddle_json_member(ev.p, ev.len, "id", &iv) == 0 && puddle_json_string(iv, idt, sizeof idt) == 0);
                    CHECK(puddle_json_member(ev.p, ev.len, "event", &tv) == 0 && puddle_json_member(ev.p, ev.len, "sig", &sv) == 0);
                    evt = malloc(tv.len + 1);
                    CHECK(puddle_json_string(tv, evt, tv.len + 1) == 0 && puddle_json_string(sv, sigt, sizeof sigt) == 0);
                    puddle_sha256_hex(evt, strlen(evt), hash);
                    CHECK(!strcmp(hash, idt));                                                   /* the message is unchanged: same id */
                    CHECK(puddle_event_sig_parse(sigt, kid, sg) == 0 && !strcmp(kid, mykid) && puddle_event_verify(pub, evt, strlen(evt), sg));
                    free(evt);
                    seen++;
                }
                CHECK(seen == 2);
            }
            free(eb);
            free(new_chain);

            /* a second change before they were told: the waiting news is brought up to date, not doubled */
            roll(&me);
            CHECK(puddle_sync_after_roll(&ctx) >= 0);
            CHECK(queued(alice.uid, "identity") == 1);
            new_chain = chain_of(&me);
            ib = item_body(alice.uid, "identity", NULL);
            got = malloc(strlen(ib) + 1);
            CHECK(puddle_json_member(ib, strlen(ib), "chain", &cv) == 0 && puddle_json_string(cv, got, strlen(ib) + 1) == 0 && !strcmp(got, new_chain));
            free(got); free(ib); free(new_chain);
            CHECK(puddle_sync_after_roll(&ctx) == 0);                                            /* and nothing more to do */
        }

        /* how it turned out: the peer is marked as holding what was delivered */
        {
            uint64_t seq;
            char *ib = item_body(alice.uid, "identity", &seq);
            puddle_ob_item it;
            memset(&it, 0, sizeof it);
            snprintf(it.peer, sizeof it.peer, "%s", alice.uid);
            snprintf(it.kind, sizeof it.kind, "identity");
            it.body = ib;
            it.body_len = strlen(ib);
            CHECK(sent_of(&alice, 0) == 0);
            puddle_sync_done(&ctx, &it, PUDDLE_OB_HOLD);
            CHECK(sent_of(&alice, 0) == 0);                                                      /* held: not yet */
            puddle_sync_done(&ctx, &it, PUDDLE_OB_DELIVERED);
            CHECK(sent_of(&alice, 0) == 2);                                                      /* two key changes ago to now */
            puddle_sync_done(&ctx, &it, PUDDLE_OB_DELIVERED);
            CHECK(sent_of(&alice, 0) == 2);
            snprintf(it.reason, sizeof it.reason, "they answered HTTP 404");
            puddle_sync_done(&ctx, &it, PUDDLE_OB_REJECTED);
            puddle_stream rec;
            CHECK(puddle_streams_get(ss, alice.uid, &rec) == 0 && strstr(rec.error, "new keys") && strstr(rec.error, "HTTP 404") && rec.chain_sent == 2);
            puddle_stream_free(&rec);
            free(ib);
            /* someone no longer holding... an unknown peer is ignored */
            snprintf(it.peer, sizeof it.peer, "%s", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
            it.body = strdup("{\"chain\":\"x\"}");
            it.body_len = strlen(it.body);
            puddle_sync_done(&ctx, &it, PUDDLE_OB_DELIVERED);
            free(it.body);
        }

        /* the items that carry our chain and address: delivered, the peer now holds them */
        {
            clear_outbox();
            char *chain = chain_of(&me);
            puddle_jw w;
            char tmp[3000], body[4000];
            puddle_jw_init(&w, tmp, sizeof tmp);
            puddle_jw_string(&w, chain);
            puddle_jw_finish(&w);
            snprintf(body, sizeof body, "{\"responder\":{\"id\":\"%s\",\"address\":\"127.0.0.1:8443\",\"profile\":{\"display_name\":\"Me\"},\"chain\":%s},\"approved\":true}", me.uid, tmp);
            puddle_ob_item it;
            memset(&it, 0, sizeof it);
            snprintf(it.peer, sizeof it.peer, "%s", erin.uid);
            snprintf(it.kind, sizeof it.kind, "result");
            it.body = body;
            it.body_len = strlen(body);
            set_peer(&erin, PUDDLE_STREAM_ACTIVE, -1, -1);
            puddle_sync_done(&ctx, &it, PUDDLE_OB_DELIVERED);
            CHECK(sent_of(&erin, 0) == 2 && sent_of(&erin, 1) == 0);                            /* the chain at its seq, and where we are */
            snprintf(body, sizeof body, "{\"responder\":{\"id\":\"%s\",\"address\":\"10.9.9.9:1\",\"profile\":{\"display_name\":\"Me\"},\"chain\":%s},\"approved\":true}", me.uid, tmp);
            set_peer(&erin, PUDDLE_STREAM_ACTIVE, -1, -1);
            it.body_len = strlen(body);
            puddle_sync_done(&ctx, &it, PUDDLE_OB_DELIVERED);
            CHECK(sent_of(&erin, 0) == 2 && sent_of(&erin, 1) == -1);                           /* a different address than the one on record: unknown */
            it.body_len = strlen(body);
            set_peer(&erin, PUDDLE_STREAM_ACTIVE, -1, -1);
            puddle_sync_done(&ctx, &it, PUDDLE_OB_REJECTED);                                    /* a refused answer is the flow's business, not ours */
            CHECK(sent_of(&erin, 0) == -1 && sent_of(&erin, 1) == -1);
            {
                puddle_stream er;
                CHECK(puddle_streams_get(ss, erin.uid, &er) == 0 && !er.error[0]);                /* and nothing is said about it here */
                puddle_stream_free(&er);
            }
            free(chain);
        }

        /* ---- telling them: after where we are changes ---- */
        {
            clear_outbox();
            set_peer(&erin, PUDDLE_STREAM_CLOSED, 2, 0);
            set_peer(&alice, PUDDLE_STREAM_ACTIVE, 2, 0);
            set_peer(&carol, PUDDLE_STREAM_PENDING_OUT, 2, -1);
            set_peer(&dave, PUDDLE_STREAM_CLOSED, 2, 0);
            CHECK(puddle_sync_resume(&ctx) == 0);                                                /* nothing changed: nothing to say */
            snprintf(cfg.advertise, sizeof cfg.advertise, "198.51.100.77:9443");
            CHECK(puddle_sync_resume(&ctx) == 1 && queued(alice.uid, "address") == 1);           /* only active peers get a record */
            CHECK(queued(carol.uid, "address") == 0 && queued(dave.uid, "address") == 0 && queued(alice.uid, "identity") == 0);
            uint64_t seq;
            char *ab = item_body(alice.uid, "address", &seq);
            {
                puddle_span rv, sv, v;
                char rec[1000], sigt[PUDDLE_SIG_TEXT_MAX + 4], kid[65];
                unsigned char sg[64], pub[32];
                CHECK(ab && puddle_json_member(ab, strlen(ab), "record", &rv) == 0 && puddle_json_string(rv, rec, sizeof rec) == 0);
                CHECK(puddle_json_member(ab, strlen(ab), "sig", &sv) == 0 && puddle_json_string(sv, sigt, sizeof sigt) == 0 && puddle_event_sig_parse(sigt, kid, sg) == 0);
                puddle_identity_public(me.id, pub, NULL, NULL);
                CHECK(puddle_text_verify(pub, PUDDLE_ADDRESS_DOMAIN, rec, strlen(rec), sg));
                CHECK(!puddle_event_verify(pub, rec, strlen(rec), sg));                         /* an address signature is not an event signature */
                int64_t sq = 0;
                CHECK(puddle_json_member(rec, strlen(rec), "seq", &v) == 0 && puddle_json_int64(v, &sq) == 0 && sq == 1);
                CHECK(strstr(rec, "\"198.51.100.77:9443\"") && strstr(rec, "\"tls_fp\":\"1111") && strstr(rec, me.uid) && strstr(rec, "\"type\":\"address\""));
            }
            free(ab);
            CHECK(puddle_sync_resume(&ctx) == 0);                                                /* told, or waiting to be: not twice */
            /* a new certificate */
            snprintf(ctx.own_tls_fp, sizeof ctx.own_tls_fp, "%s", "4444444444444444444444444444444444444444444444444444444444444444");
            CHECK(puddle_sync_resume(&ctx) == 0 && queued(alice.uid, "address") == 1);           /* the waiting record is not doubled... */
            clear_outbox();
            CHECK(puddle_sync_resume(&ctx) == 1);                                                /* ...and once it is gone, the next is made */
            ab = item_body(alice.uid, "address", &seq);
            CHECK(ab && strstr(ab, "address"));
            {
                puddle_span rv, v;
                char rec[1000];
                int64_t sq = 0;
                CHECK(puddle_json_member(ab, strlen(ab), "record", &rv) == 0 && puddle_json_string(rv, rec, sizeof rec) == 0);
                CHECK(puddle_json_member(rec, strlen(rec), "seq", &v) == 0 && puddle_json_int64(v, &sq) == 0 && sq == 2);   /* the address, then the certificate: two records on */
            }
            free(ab);
            /* delivery marks it */
            puddle_ob_item *l;
            size_t n;
            puddle_outbox_list(ob, &l, &n);
            CHECK(n == 1 && !strcmp(l[0].kind, "address"));
            puddle_sync_done(&ctx, &l[0], PUDDLE_OB_DELIVERED);
            puddle_outbox_list_free(l, n);
            CHECK(sent_of(&alice, 1) >= 1);
            /* a damaged record of what we said before: start again from what is true now, without a fuss */
            snprintf(path, sizeof path, "%s/state/address.json", data);
            FILE *f = fopen(path, "wb");
            fputs("garbage", f);
            fclose(f);
            clear_outbox();
            CHECK(puddle_sync_resume(&ctx) == 0);
            CHECK(puddle_sync_resume(&ctx) == 0);
        }
        puddle_identity_close(dave.id);
        puddle_identity_close(erin.id);
    }

    /* ---- an announced certificate frees what was held for it; an old failure is left as it was ---- */
    {
        clear_outbox();
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, 99, 99);
        write_outbox_item(1, alice.uid, "events", "/stream/v1/events", "{\"chat\":\"x\",\"events\":[]}", "held");
        write_outbox_item(2, alice.uid, "access", "/stream/v1/access", "{\"requester\":{\"chain\":\"old\"}}", "failed");
        reopen_outbox();
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 2 && l[0].state == PUDDLE_OB_HELD && l[1].state == PUDDLE_OB_FAILED);
        puddle_outbox_list_free(l, n);
        /* a record without a certificate in it does not release them */
        char *nb = address_body(&alice, alice.uid, "address", "[\"192.0.2.50:5\"]", NULL, 31, T0 + 1);
        CHECK(post_as(&alice, "/stream/v1/address", nb, 1).status == 204);
        free(nb);
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 2 && l[0].state == PUDDLE_OB_HELD);
        puddle_outbox_list_free(l, n);
        nb = address_body(&alice, alice.uid, "address", "[\"192.0.2.51:5\"]", "\"tls_fp\":\"5555555555555555555555555555555555555555555555555555555555555555\"", 32, T0 + 2);
        CHECK(post_as(&alice, "/stream/v1/address", nb, 1).status == 204);
        free(nb);
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 2 && l[0].state == PUDDLE_OB_QUEUED && l[1].state == PUDDLE_OB_FAILED);        /* released, and the failure is still a failure */
        puddle_outbox_list_free(l, n);
        /* after a change of keys, what failed is not rewritten: it is over */
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, 99, 99);
        roll(&me);
        puddle_sync_after_roll(&ctx);
        puddle_outbox_list(ob, &l, &n);
        int found = 0;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(l[i].kind, "access")) {
                found = 1;
                CHECK(l[i].state == PUDDLE_OB_FAILED && !strcmp(l[i].body, "{\"requester\":{\"chain\":\"old\"}}"));
            }
        CHECK(found);
        puddle_outbox_list_free(l, n);
    }

    puddle_outbox_close(ob);
    puddle_streams_close(ss);
    puddle_replay_free(ctx.replay);
    puddle_identity_close(me.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
