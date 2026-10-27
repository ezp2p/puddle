#include "federation/keysync.h"

#include "federation/access.h"
#include "http/client.h"
#include "base/encoding.h"
#include "federation/flow.h"
#include "platform/fsutil.h"
#include "base/hash.h"
#include "base/log.h"
#include "events/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers ---- */

typedef struct {
    char *p;
    size_t len, cap;
    int bad;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->bad)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *np = (char *)realloc(b->p, cap);
        if (!np) {
            b->bad = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

/* `text` as a JSON string, into `b` */
static void sb_add_json_string(sbuf *b, const char *text, size_t n)
{
    size_t cap = n * 6 + 16;
    char *tmp = (char *)malloc(cap);
    if (!tmp) {
        b->bad = 1;
        return;
    }
    puddle_jw w;
    puddle_jw_init(&w, tmp, cap);
    puddle_jw_string_n(&w, text, n);
    size_t m = puddle_jw_finish(&w);
    if (m)
        sb_add(b, tmp, m);
    else
        b->bad = 1;
    free(tmp);
}

static int64_t my_seq(puddle_ctx *ctx)
{
    uint64_t seq = 0;
    puddle_identity_public(ctx->identity, NULL, NULL, &seq);
    return (int64_t)seq;
}

/* the seq at the end of a chain text, or -1 if it does not verify */
static int64_t chain_seq(const char *chain, size_t len)
{
    puddle_chain_head h;
    char err[200];
    if (puddle_chain_verify(chain, len, NULL, &h, err, sizeof err) != 0)
        return -1;
    return (int64_t)h.seq;
}

/* Is `longer` our `shorter` plus more whole lines? */
static int extends(const char *shorter, size_t sl, const char *longer, size_t ll)
{
    if (ll <= sl || memcmp(shorter, longer, sl) != 0)
        return 0;
    return sl > 0 && (shorter[sl - 1] == '\n' || longer[sl] == '\n');
}

/* ---- our address record: what we last said about where we are ---- */

typedef struct {
    int64_t seq;
    char address[PUDDLE_ADDR_MAX];
    char tls_fp[65];
} address_state;

static void state_path(const puddle_ctx *ctx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/state/address.json", ctx->config->data_dir);
}

static int load_state(const puddle_ctx *ctx, address_state *st)
{
    char path[1200];
    state_path(ctx, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    char text[1024];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    puddle_span v;
    memset(st, 0, sizeof *st);
    if (!puddle_json_valid_object(text, n) || puddle_json_member(text, n, "seq", &v) != 0 || puddle_json_int64(v, &st->seq) != 0 ||
        st->seq < 0 || puddle_json_member(text, n, "address", &v) != 0 || puddle_json_string(v, st->address, sizeof st->address) != 0 ||
        puddle_json_member(text, n, "tls_fp", &v) != 0 || puddle_json_string(v, st->tls_fp, sizeof st->tls_fp) != 0)
        return 1; /* damaged: start again from what is true now */
    return 0;
}

static int save_state(const puddle_ctx *ctx, const address_state *st, int64_t now)
{
    char buf[1024], path[1200], dir[1100];
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "seq", st->seq);
    puddle_jw_str(&w, "address", st->address);
    puddle_jw_str(&w, "tls_fp", st->tls_fp);
    puddle_jw_num(&w, "ts", now);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    snprintf(dir, sizeof dir, "%s/state", ctx->config->data_dir);
    state_path(ctx, path, sizeof path);
    return n && puddle_write_file_atomic(dir, path, buf, n) == 0 ? 0 : -1;
}

/* the signed record for `st`: {"record":"<text>","sig":"<kid:b64>"}; malloc'd, or NULL */
static char *address_body(puddle_ctx *ctx, const address_state *st, int64_t now, size_t *len)
{
    char me[PUDDLE_USER_ID_LEN + 1], rec[1024], sig[PUDDLE_SIG_TEXT_MAX];
    puddle_identity_id(ctx->identity, me);
    puddle_jw w;
    puddle_jw_init(&w, rec, sizeof rec);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", "address");
    puddle_jw_str(&w, "id", me);
    puddle_jw_key(&w, "addresses");
    puddle_jw_array(&w);
    if (st->address[0])
        puddle_jw_string(&w, st->address);
    puddle_jw_end_array(&w);
    if (st->tls_fp[0])
        puddle_jw_str(&w, "tls_fp", st->tls_fp);
    puddle_jw_num(&w, "seq", st->seq);
    puddle_jw_num(&w, "ts", now);
    puddle_jw_end_object(&w);
    size_t rl = puddle_jw_finish(&w);
    if (!rl || puddle_text_sign(ctx->identity, PUDDLE_ADDRESS_DOMAIN, rec, rl, sig) != 0)
        return NULL;
    sbuf b = {};
    sb_add(&b, "{\"record\":", 10);
    sb_add_json_string(&b, rec, rl);
    sb_add(&b, ",\"sig\":", 7);
    sb_add_json_string(&b, sig, strlen(sig));
    sb_add(&b, "}", 1);
    if (b.bad) {
        free(b.p);
        return NULL;
    }
    *len = b.len;
    return b.p;
}

/* ---- bringing queued things up to date after a key change ---- */

/* `body` with the string value of "chain" inside its `key` object replaced by `chain`; or NULL if it is already that */
static char *body_with_chain(const char *body, size_t len, const char *key, const char *chain, size_t cl, size_t *out_len)
{
    puddle_span obj, cv;
    if (puddle_json_member(body, len, key, &obj) != 0 || puddle_json_member(obj.p, obj.len, "chain", &cv) != 0 || cv.len < 2 || cv.p[0] != '"')
        return NULL;
    char *old = (char *)malloc(cv.len + 1);
    if (!old)
        return NULL;
    int same = puddle_json_string(cv, old, cv.len + 1) == 0 && strlen(old) == cl && !memcmp(old, chain, cl);
    free(old);
    if (same)
        return NULL;
    sbuf b = {};
    sb_add(&b, body, (size_t)(cv.p - body));
    sb_add_json_string(&b, chain, cl);
    sb_add(&b, cv.p + cv.len, (size_t)(body + len - (cv.p + cv.len)));
    if (b.bad) {
        free(b.p);
        return NULL;
    }
    *out_len = b.len;
    return b.p;
}

/* an events delivery with each signature that is not by our current key made again by it; NULL if none needed it */
static char *body_resigned(puddle_ctx *ctx, const char *body, size_t len, size_t *out_len)
{
    unsigned char pub[32];
    char kid[65];
    puddle_identity_public(ctx->identity, pub, NULL, NULL);
    puddle_key_id(pub, kid);
    puddle_span evs, ev;
    if (puddle_json_member(body, len, "events", &evs) != 0)
        return NULL;
    sbuf b = {};
    size_t cursor = 0, done = 0;
    int changed = 0, r;
    while ((r = puddle_json_array_next(evs, &cursor, &ev)) == 1) {
        puddle_span sv, tv;
        char sigtext[PUDDLE_SIG_TEXT_MAX + 4];
        if (puddle_json_member(ev.p, ev.len, "sig", &sv) != 0 || puddle_json_string(sv, sigtext, sizeof sigtext) != 0 ||
            puddle_json_member(ev.p, ev.len, "event", &tv) != 0 || tv.len < 2)
            continue;
        if (!strncmp(sigtext, kid, 64) && sigtext[64] == ':')
            continue; /* already ours, as we are now */
        char *event = (char *)malloc(tv.len + 1);
        char fresh[PUDDLE_SIG_TEXT_MAX];
        if (!event || puddle_json_string(tv, event, tv.len + 1) != 0 || puddle_event_sign(ctx->identity, event, strlen(event), fresh) != 0) {
            free(event);
            continue;
        }
        free(event);
        sb_add(&b, body + done, (size_t)(sv.p - body) - done);
        sb_add_json_string(&b, fresh, strlen(fresh));
        done = (size_t)(sv.p - body) + sv.len;
        changed = 1;
    }
    if (!changed || r < 0 || b.bad) {
        free(b.p);
        return NULL;
    }
    sb_add(&b, body + done, len - done);
    if (b.bad) {
        free(b.p);
        return NULL;
    }
    *out_len = b.len;
    return b.p;
}

static int refresh_queued(puddle_ctx *ctx, size_t cl, const char *chain)
{
    puddle_ob_item *items;
    size_t n;
    int changed = 0;
    if (puddle_outbox_list(ctx->outbox, &items, &n) != 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (items[i].state == PUDDLE_OB_FAILED)
            continue;
        char *nb = NULL;
        size_t nl = 0;
        if (!strcmp(items[i].kind, "access"))
            nb = body_with_chain(items[i].body, items[i].body_len, "requester", chain, cl, &nl);
        else if (!strcmp(items[i].kind, "result"))
            nb = body_with_chain(items[i].body, items[i].body_len, "responder", chain, cl, &nl);
        else if (!strcmp(items[i].kind, "identity")) {
            puddle_span cv;
            char *old = NULL;
            if (puddle_json_member(items[i].body, items[i].body_len, "chain", &cv) == 0 && (old = (char *)malloc(cv.len + 1)) &&
                puddle_json_string(cv, old, cv.len + 1) == 0 && (strlen(old) != cl || memcmp(old, chain, cl) != 0)) {
                sbuf b = {};
                sb_add(&b, "{\"chain\":", 9);
                sb_add_json_string(&b, chain, cl);
                sb_add(&b, "}", 1);
                if (!b.bad) { nb = b.p; nl = b.len; } else free(b.p);
            }
            free(old);
        } else if (!strcmp(items[i].kind, "events"))
            nb = body_resigned(ctx, items[i].body, items[i].body_len, &nl);
        if (nb) {
            if (puddle_outbox_rewrite(ctx->outbox, items[i].seq, nb, nl) == 0)
                changed++;
            free(nb);
        }
    }
    puddle_outbox_list_free(items, n);
    return changed;
}

/* ---- telling peers ---- */

static int waiting_for(puddle_ob_item *items, size_t n, const char *peer, const char *kind)
{
    for (size_t i = 0; i < n; i++)
        if (items[i].state != PUDDLE_OB_FAILED && !strcmp(items[i].peer, peer) && !strcmp(items[i].kind, kind))
            return 1;
    return 0;
}

static int push_all(puddle_ctx *ctx, const address_state *as, int64_t now)
{
    puddle_stream *all;
    size_t n;
    puddle_ob_item *items;
    size_t ni;
    int queued = 0;
    if (puddle_streams_list(ctx->streams, &all, &n) != 0)
        return 0;
    if (puddle_outbox_list(ctx->outbox, &items, &ni) != 0) {
        puddle_stream_list_free(all, n);
        return 0;
    }
    size_t cl;
    char *chain = puddle_identity_chain(ctx->identity, &cl);
    int64_t seq = my_seq(ctx);
    char err[300];
    for (size_t i = 0; i < n; i++) {
        const puddle_stream *s = &all[i];
        if (s->provisional)
            continue; /* we only know where they are, not who: nothing to push to a made-up id */
        int holds_chain = puddle_stream_carries_groups(s) || s->state == PUDDLE_STREAM_PENDING_OUT;
        if (chain && holds_chain && s->chain_sent < seq && !waiting_for(items, ni, s->id, "identity")) {
            sbuf b = {};
            sb_add(&b, "{\"chain\":", 9);
            sb_add_json_string(&b, chain, cl);
            sb_add(&b, "}", 1);
            if (!b.bad && puddle_outbox_enqueue(ctx->outbox, s->id, "identity", "/stream/v1/identity", b.p, b.len, err, sizeof err) == 0)
                queued++;
            free(b.p);
        }
        if (puddle_stream_carries_groups(s) && s->addr_sent < as->seq && !waiting_for(items, ni, s->id, "address")) {
            size_t bl;
            char *body = address_body(ctx, as, now, &bl);
            if (body && puddle_outbox_enqueue(ctx->outbox, s->id, "address", "/stream/v1/address", body, bl, err, sizeof err) == 0)
                queued++;
            free(body);
        }
    }
    free(chain);
    puddle_outbox_list_free(items, ni);
    puddle_stream_list_free(all, n);
    return queued;
}

static int bring_up_to_date(puddle_ctx *ctx, const address_state *as)
{
    if (!ctx->outbox || !ctx->streams || !ctx->identity)
        return 0;
    size_t cl;
    char *chain = puddle_identity_chain(ctx->identity, &cl);
    int n = 0;
    if (chain) {
        n += refresh_queued(ctx, cl, chain); /* before queueing: what is pushed is already current */
        free(chain);
    }
    n += push_all(ctx, as, puddle_ctx_now_ms(ctx));
    return n;
}

int puddle_sync_after_roll(puddle_ctx *ctx)
{
    address_state as;
    if (!ctx->config || load_state(ctx, &as) != 0) {
        memset(&as, 0, sizeof as);
        as.seq = 0;
    }
    return bring_up_to_date(ctx, &as);
}

int puddle_sync_resume(puddle_ctx *ctx)
{
    if (!ctx->config)
        return 0;
    address_state saved, now_state;
    char addr[PUDDLE_ADDR_MAX] = "";
    puddle_my_address(ctx->config, addr, sizeof addr);
    memset(&now_state, 0, sizeof now_state);
    snprintf(now_state.address, sizeof now_state.address, "%s", addr);
    snprintf(now_state.tls_fp, sizeof now_state.tls_fp, "%s", ctx->own_tls_fp);
    int have = load_state(ctx, &saved) == 0;
    int64_t now = puddle_ctx_now_ms(ctx);
    if (!have) {
        now_state.seq = 0; /* the first record: people were told where we are when the stream began */
        save_state(ctx, &now_state, now);
    } else if (strcmp(saved.address, now_state.address) != 0 || strcmp(saved.tls_fp, now_state.tls_fp) != 0) {
        now_state.seq = saved.seq + 1;
        if (save_state(ctx, &now_state, now) != 0)
            puddle_log(PUDDLE_LOG_WARN, "cannot save our address record; friends will not be told it changed");
        puddle_log(PUDDLE_LOG_INFO, "our address or certificate changed: telling the people we are connected to");
    } else {
        now_state = saved;
    }
    return bring_up_to_date(ctx, &now_state);
}

/* ---- what became of it ---- */

typedef struct {
    int64_t chain_seq, addr_seq;
    const char *error;
} sent_note;

static int apply_sent(void *user, puddle_stream *st, int exists)
{
    sent_note *n = (sent_note *)user;
    if (!exists)
        return 1;
    if (n->error) {
        snprintf(st->error, sizeof st->error, "%s", n->error);
        return 0;
    }
    int changed = 0;
    if (n->chain_seq > st->chain_sent) { st->chain_sent = n->chain_seq; changed = 1; }
    if (n->addr_seq > st->addr_sent) { st->addr_sent = n->addr_seq; changed = 1; }
    return changed ? 0 : 1;
}

void puddle_sync_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome)
{
    int ours = !strcmp(item->kind, "identity") || !strcmp(item->kind, "address");
    int carries = !strcmp(item->kind, "access") || !strcmp(item->kind, "result");
    if (!ours && !carries)
        return;
    char err[300], why[200];
    sent_note n = { -1, -1, NULL };
    if (outcome == PUDDLE_OB_HOLD)
        return;
    if (outcome == PUDDLE_OB_REJECTED) {
        if (!ours)
            return; /* a refused request or answer is dealt with where it is dealt with */
        snprintf(why, sizeof why, "%s could not be told about our new %s: %.120s", "they", !strcmp(item->kind, "identity") ? "keys" : "address", item->reason);
        n.error = why;
    } else if (!strcmp(item->kind, "identity")) {
        puddle_span cv;
        if (puddle_json_member(item->body, item->body_len, "chain", &cv) != 0)
            return;
        char *chain = (char *)malloc(cv.len + 1);
        if (!chain)
            return;
        if (puddle_json_string(cv, chain, cv.len + 1) == 0)
            n.chain_seq = chain_seq(chain, strlen(chain));
        free(chain);
    } else if (!strcmp(item->kind, "address")) {
        puddle_span rv, v;
        if (puddle_json_member(item->body, item->body_len, "record", &rv) != 0)
            return;
        char *rec = (char *)malloc(rv.len + 1);
        if (!rec)
            return;
        if (puddle_json_string(rv, rec, rv.len + 1) == 0 && puddle_json_member(rec, strlen(rec), "seq", &v) == 0)
            puddle_json_int64(v, &n.addr_seq);
        free(rec);
    } else {
        /* a request or an answer carries our chain and where we are: that is the peer's picture of us from then on */
        const char *key = !strcmp(item->kind, "access") ? "requester" : "responder";
        puddle_span obj, v;
        if (puddle_json_member(item->body, item->body_len, key, &obj) != 0)
            return;
        if (puddle_json_member(obj.p, obj.len, "chain", &v) == 0) {
            char *chain = (char *)malloc(v.len + 1);
            if (chain && puddle_json_string(v, chain, v.len + 1) == 0)
                n.chain_seq = chain_seq(chain, strlen(chain));
            free(chain);
        }
        address_state as;
        char addr[PUDDLE_ADDR_MAX];
        if (ctx->config && load_state(ctx, &as) == 0 && puddle_json_member(obj.p, obj.len, "address", &v) == 0 &&
            puddle_json_string(v, addr, sizeof addr) == 0 && !strcmp(addr, as.address))
            n.addr_seq = as.seq;
    }
    puddle_streams_update(ctx->streams, item->peer, apply_sent, &n, err, sizeof err);
}

/* ---- inbound: they changed keys ---- */

static int chain_from_body(const unsigned char *body, size_t len, char **chain, size_t *cl)
{
    puddle_span v;
    if (puddle_json_member((const char *)body, len, "chain", &v) != 0 || v.len < 2 || v.p[0] != '"' || v.len > PUDDLE_ACCESS_CHAIN_MAX * 6u)
        return -1;
    char *c = (char *)malloc(v.len + 1);
    if (!c || puddle_json_string(v, c, v.len + 1) != 0) {
        free(c);
        return -1;
    }
    *chain = c;
    *cl = strlen(c);
    if (*cl == 0 || *cl > PUDDLE_ACCESS_CHAIN_MAX) {
        free(c);
        return -1;
    }
    return 0;
}

static int identity_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t len, const char *sender, const char *kid,
                              unsigned char sign_pub[32])
{
    if (!puddle_id_valid(sender) || !kid)
        return 0;
    if (ctx->anon_bucket && !puddle_bucket_take(ctx->anon_bucket, puddle_ctx_now_ms(ctx)))
        return 0;
    char *chain;
    size_t cl;
    if (chain_from_body(body, len, &chain, &cl) != 0)
        return 0;
    puddle_chain_head h;
    char err[200], want[65];
    int ok = puddle_chain_verify(chain, cl, sender, &h, err, sizeof err) == 0;
    free(chain);
    if (!ok)
        return 0;
    puddle_key_id(h.sign_pub, want);
    if (strcmp(want, kid) != 0)
        return 0;
    memcpy(sign_pub, h.sign_pub, 32);
    return 1;
}

typedef struct {
    const char *chain;
    size_t len;
} newer_chain;

static int apply_chain(void *user, puddle_stream *st, int exists)
{
    newer_chain *c = (newer_chain *)user;
    if (!exists || !st->chain || !extends(st->chain, st->chain_len, c->chain, c->len))
        return 1; /* not someone we hold keys for, or not a continuation of what we hold: nothing to change */
    return puddle_stream_set_chain(st, c->chain, c->len) == 0 ? 0 : -1;
}

/* A stranger can sign with a chain of their own, so this answers `202` whatever it makes of it: what counts is that
 * the chain continues the one we already hold for that id, which only the owner of the keys can produce. */
static int identity_post(const puddle_call *c, puddle_jw *w)
{
    (void)w;
    char *chain;
    size_t cl;
    if (chain_from_body(c->body, c->body_len, &chain, &cl) != 0)
        return 202;
    newer_chain nc = { chain, cl };
    char err[300];
    if (!puddle_streams_is_blocked(c->ctx->streams, c->sender) &&
        puddle_streams_update(c->ctx->streams, c->sender, apply_chain, &nc, err, sizeof err) == 0)
        puddle_log(PUDDLE_LOG_INFO, "%.12s… changed keys", c->sender);
    free(chain);
    return 202;
}

/* ---- inbound: they moved ---- */

typedef struct {
    int64_t seq;
    char addrs[PUDDLE_STREAM_MAX_ADDRS][PUDDLE_ADDR_MAX];
    int naddr;
    char tls_fp[65];
    int newer;
} moved;

static int apply_moved(void *user, puddle_stream *st, int exists)
{
    moved *m = (moved *)user;
    if (!exists || m->seq <= st->addr_seq)
        return 1; /* the highest seq wins; this is not newer than what we hold */
    st->addr_seq = m->seq;
    if (m->naddr > 0) {
        st->naddr = 0;
        for (int i = 0; i < m->naddr; i++)
            puddle_stream_add_address(st, m->addrs[i]);
    }
    if (m->tls_fp[0]) {
        /* signed by their current key, so a new certificate announced here is theirs: accept it without asking */
        snprintf(st->tls_fp, sizeof st->tls_fp, "%s", m->tls_fp);
        st->tls_fp_seen[0] = '\0';
    }
    m->newer = 1;
    return 0;
}

static int address_post(const puddle_call *c, puddle_jw *w)
{
    puddle_span rv, sv, v;
    const char *b = (const char *)c->body;
    char sigtext[PUDDLE_SIG_TEXT_MAX + 4], kid[65];
    unsigned char sig[64];
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "record", &rv) != 0 || rv.len < 2 || rv.p[0] != '"' ||
        puddle_json_member(b, c->body_len, "sig", &sv) != 0 || puddle_json_string(sv, sigtext, sizeof sigtext) != 0 ||
        puddle_event_sig_parse(sigtext, kid, sig) != 0)
        return puddle_fail(w, 400, "bad_request");
    char *rec = (char *)malloc(rv.len + 1);
    if (!rec || puddle_json_string(rv, rec, rv.len + 1) != 0) {
        free(rec);
        return puddle_fail(w, 400, "bad_request");
    }
    size_t rl = strlen(rec);
    moved m;
    memset(&m, 0, sizeof m);
    char type[16], id[PUDDLE_USER_ID_LEN + 1 + 8];
    int64_t ts;
    puddle_span av;
    int ok = rl <= 2048 && puddle_json_valid_object(rec, rl) && puddle_json_member(rec, rl, "type", &v) == 0 &&
             puddle_json_string(v, type, sizeof type) == 0 && !strcmp(type, "address") && puddle_json_member(rec, rl, "id", &v) == 0 &&
             puddle_json_string(v, id, sizeof id) == 0 && !strcmp(id, c->sender) && puddle_json_member(rec, rl, "seq", &v) == 0 &&
             puddle_json_int64(v, &m.seq) == 0 && m.seq >= 0 && puddle_json_member(rec, rl, "ts", &v) == 0 && puddle_json_int64(v, &ts) == 0 &&
             ts > 946684800000LL && ts < 4102444800000LL && puddle_json_member(rec, rl, "addresses", &av) == 0;
    if (ok) {
        size_t cur = 0;
        puddle_span it;
        int r;
        while ((r = puddle_json_array_next(av, &cur, &it)) == 1) {
            char t[PUDDLE_ADDR_MAX + 4];
            puddle_address pa;
            if (m.naddr >= PUDDLE_STREAM_MAX_ADDRS || puddle_json_string(it, t, sizeof t) != 0 || puddle_address_parse(t, &pa) != 0 ||
                !puddle_net_host_plausible(pa.host)) {
                ok = 0;
                break;
            }
            snprintf(m.addrs[m.naddr++], PUDDLE_ADDR_MAX, "%s", pa.address);
        }
        if (r < 0)
            ok = 0;
        int frc = puddle_json_member(rec, rl, "tls_fp", &v);
        unsigned char raw[32];
        if (frc < 0 || (frc == 0 && (puddle_json_string(v, m.tls_fp, sizeof m.tls_fp) != 0 || puddle_hex_decode(m.tls_fp, raw, 32) != 0)))
            ok = 0;
    }
    /* it must be signed by their key as we hold it now: the request already was, and the record has to agree */
    if (ok) {
        puddle_stream st;
        ok = 0;
        if (puddle_streams_get(c->ctx->streams, c->sender, &st) == 0) {
            puddle_chain_head h;
            char err[200], want[65];
            if (st.chain && puddle_chain_verify(st.chain, st.chain_len, c->sender, &h, err, sizeof err) == 0) {
                puddle_key_id(h.sign_pub, want);
                ok = !strcmp(want, kid) && puddle_text_verify(h.sign_pub, PUDDLE_ADDRESS_DOMAIN, rec, rl, sig);
            }
            puddle_stream_free(&st);
        }
    }
    free(rec);
    if (!ok)
        return puddle_fail(w, 400, "bad_record");
    char err[300];
    int rc = puddle_streams_update(c->ctx->streams, c->sender, apply_moved, &m, err, sizeof err);
    if (rc == 0) {
        puddle_log(PUDDLE_LOG_INFO, "%.12s… has a new address%s", c->sender, m.tls_fp[0] ? " or certificate" : "");
        if (m.tls_fp[0] && c->ctx->outbox)
            puddle_outbox_release(c->ctx->outbox, c->sender); /* whatever was held for their certificate may go now */
    }
    return 204;
}

const puddle_route puddle_sync_routes[] = {
    { "POST", "/stream/v1/identity", PUDDLE_AUTH_CHAIN, PUDDLE_ACCESS_BODY_MAX, identity_post, identity_chain_key },
    { "POST", "/stream/v1/address",  PUDDLE_AUTH_PEER,  8192, address_post, NULL },
};
const size_t puddle_sync_routes_count = sizeof puddle_sync_routes / sizeof puddle_sync_routes[0];
